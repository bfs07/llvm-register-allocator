//===-- RAColorBasedCoalescing.cpp - Color-based Coalescing Register Allocator ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the MIT License.
// See the LICENSE file for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the RAColorBasedCoalescing function pass, which provides
// a implementation of the Coloring-based coalescing register allocator.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "CodeGen/AllocationOrder.h"
#include "CodeGen/LiveDebugVariables.h"
#include "CodeGen/SplitKit.h"
#include "CodeGen/RegAllocBase.h"
#include "CodeGen/Spiller.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStackAnalysis.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <cstdlib>
#include <map>
#include <set>
#include <stack>
#include <queue>
#include <list>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

#define COLOR_INVALID 0

namespace llvm {
  FunctionPass *createColorBasedRegAlloc();
}

static RegisterRegAlloc colorBasedCoalescingRegAlloc("colorBased",
                                                     "color-based coalescing register allocator",
                                                     createColorBasedRegAlloc);

namespace {
  struct CompSpillWeight {
    bool operator()(LiveInterval *A, LiveInterval *B) const {
      return A->weight < B->weight;
    }
  };
}

namespace {

  //LLVM
  MachineBlockFrequencyInfo *MBFI;
  MachineDominatorTree *DomTree;
  MachineLoopInfo *MLI;
  LiveDebugVariables *DebugVars;
  AliasAnalysis *AA;

  std::unique_ptr<SplitAnalysis> SA;
  std::unique_ptr<SplitEditor> SE;


  // Graph Coloring
  std::map<unsigned, std::set<unsigned>> InterferenceGraph;
  std::map<unsigned, int> Degree;
  std::map<unsigned, bool> OnStack;
  std::priority_queue<std::pair<unsigned, unsigned>> ColoringPq;
  std::map<unsigned, int> ColorsTemp;
  std::map<unsigned, int> Colors;
  std::map<unsigned, std::set<unsigned>> CopyRelated;
  std::vector<int> ExtendedColors;
  std::map<unsigned, double> SpillWeight;


  class RAColorBasedCoalescing : public MachineFunctionPass, public RegAllocBase {
    // context
    MachineFunction *MF;

    // state
    std::unique_ptr<Spiller> SpillerInstance;
    std::priority_queue<LiveInterval*, std::vector<LiveInterval*>,
                        CompSpillWeight> Queue;

    // Scratch space.  Allocated here to avoid repeated malloc calls in
    // selectOrSplit().
    BitVector UsableRegs;

    private:

      void algorithm(MachineFunction &mf);

      void calculateSpillCosts();

      void clearAll();

      bool spillInterferences(LiveInterval &VirtReg, unsigned PhysReg, SmallVectorImpl<unsigned> &SplitVRegs);

      bool isMarkedForSpill(unsigned vreg);

      int getNumPhysicalRegs(unsigned VirtRegID);
      
      void printVirtualRegisters();

      // ===-------------- Interference Graph methods --------------===

      void buildInterferenceGraph();

      void printInterferenceGraph();

      void printInterferenceGraphWithColor();

      // ===-------------- Coloring methods --------------===

      void simplify();

      void biasedSelectExtended();

      bool isExtendedColor(int color);

      std::vector<int> getPotentialRegs(unsigned vreg);

      int getColor(std::vector<int> Colors, unsigned vreg);

      int createNewExtendedColor();

    public:
      RAColorBasedCoalescing();

      /// Return the pass name.
      StringRef getPassName() const override { return "Color-based Coalescing Register Allocator"; }

      /// RAColorBasedCoalescing analysis usage.
      void getAnalysisUsage(AnalysisUsage &AU) const override;

      void releaseMemory() override;

      Spiller &spiller() override { return *SpillerInstance; }

      void enqueue(LiveInterval *LI) override {
        Queue.push(LI);
      }

      LiveInterval *dequeue() override {
        if (Queue.empty())
          return nullptr;
        LiveInterval *LI = Queue.top();
        Queue.pop();
        return LI;
      }

      unsigned selectOrSplit(LiveInterval &VirtReg, SmallVectorImpl<unsigned> &SplitVRegs) override;

      /// Perform register allocation.
      bool runOnMachineFunction(MachineFunction &mf) override;

      MachineFunctionProperties getRequiredProperties() const override {
        return MachineFunctionProperties().set(
            MachineFunctionProperties::Property::NoPHIs);
      }

      static char ID;
  };

  char RAColorBasedCoalescing::ID = 0;

} // end anonymous namespace

RAColorBasedCoalescing::RAColorBasedCoalescing(): MachineFunctionPass(ID) {
  initializeLiveDebugVariablesPass(*PassRegistry::getPassRegistry());
  initializeLiveIntervalsPass(*PassRegistry::getPassRegistry());
  initializeSlotIndexesPass(*PassRegistry::getPassRegistry());
  initializeRegisterCoalescerPass(*PassRegistry::getPassRegistry());
  initializeMachineSchedulerPass(*PassRegistry::getPassRegistry());
  initializeLiveStacksPass(*PassRegistry::getPassRegistry());
  initializeMachineDominatorTreePass(*PassRegistry::getPassRegistry());
  initializeMachineLoopInfoPass(*PassRegistry::getPassRegistry());
  initializeVirtRegMapPass(*PassRegistry::getPassRegistry());
  initializeLiveRegMatrixPass(*PassRegistry::getPassRegistry());
}

void RAColorBasedCoalescing::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervals>();
  AU.addPreserved<LiveIntervals>();
  AU.addPreserved<SlotIndexes>();
  AU.addRequired<LiveDebugVariables>();
  AU.addPreserved<LiveDebugVariables>();
  AU.addRequired<LiveStacks>();
  AU.addPreserved<LiveStacks>();
  AU.addRequired<MachineBlockFrequencyInfo>();
  AU.addPreserved<MachineBlockFrequencyInfo>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfo>();
  AU.addPreserved<MachineLoopInfo>();
  AU.addRequired<VirtRegMap>();
  AU.addPreserved<VirtRegMap>();
  AU.addRequired<LiveRegMatrix>();
  AU.addPreserved<LiveRegMatrix>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

void RAColorBasedCoalescing::releaseMemory() {
  SpillerInstance.reset();
}

unsigned RAColorBasedCoalescing::selectOrSplit(LiveInterval &VirtReg, SmallVectorImpl<unsigned> &SplitVRegs) {
  // Populate a list of physical register spill candidates.
  SmallVector<unsigned, 8> PhysRegSpillCands;

  std::vector<int> potentialRegs = getPotentialRegs(VirtReg.reg);

  // idx represents the index of ColorsTemp[VirtReg.reg])
  int idx = std::lower_bound(potentialRegs.begin(), potentialRegs.end(), ColorsTemp[VirtReg.reg]) - potentialRegs.begin();
  // rotating in order to put the color(physical register) chosen in getColor method as first member
  std::rotate(potentialRegs.begin(), potentialRegs.begin() + idx, potentialRegs.end());

  for (unsigned PhysReg: potentialRegs) {
    // Check for interference in PhysReg
    switch (Matrix->checkInterference(VirtReg, PhysReg)) {
    case LiveRegMatrix::IK_Free:
      // PhysReg is available, allocate it.
      return PhysReg;

    case LiveRegMatrix::IK_VirtReg:
      // Only virtual registers in the way, we may be able to spill them.
      PhysRegSpillCands.push_back(PhysReg);
      continue;

    default:
      // RegMask or RegUnit interference.
      continue;
    }
  }

  // Try to spill another interfering reg with less spill weight.
  for (SmallVectorImpl<unsigned>::iterator PhysRegI = PhysRegSpillCands.begin(),
       PhysRegE = PhysRegSpillCands.end(); PhysRegI != PhysRegE; ++PhysRegI) {
    if (!spillInterferences(VirtReg, *PhysRegI, SplitVRegs))
      continue;

    assert(!Matrix->checkInterference(VirtReg, *PhysRegI) &&
           "Interference after spill.");
    // Tell the caller to allocate to this newly freed physical register.
    return *PhysRegI;
  }

  // No other spill candidates were found, so spill the current VirtReg.
  // DEBUG(dbgs() << "spilling: " << VirtReg << '\n');
  if (!VirtReg.isSpillable())
    return ~0u;
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  return 0;
}

bool RAColorBasedCoalescing::spillInterferences(LiveInterval &VirtReg, unsigned PhysReg, SmallVectorImpl<unsigned> &SplitVRegs) {
  // Record each interference and determine if all are spillable before mutating
  // either the union or live intervals.
  SmallVector<LiveInterval*, 8> Intfs;

  // Collect interferences assigned to any alias of the physical register.
  for (MCRegUnitIterator Units(PhysReg, TRI); Units.isValid(); ++Units) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, *Units);
    Q.collectInterferingVRegs();
    if (Q.seenUnspillableVReg())
      return false;
    for (unsigned i = Q.interferingVRegs().size(); i; --i) {
      LiveInterval *Intf = Q.interferingVRegs()[i - 1];
      if (!Intf->isSpillable() || Intf->weight > VirtReg.weight)
        return false;
      Intfs.push_back(Intf);
    }
  }
  /*DEBUG(dbgs() << "spilling " << TRI->getName(PhysReg) <<
        " interferences with " << VirtReg << "\n");*/
  assert(!Intfs.empty() && "expected interference");

  // Spill each interfering vreg allocated to PhysReg or an alias.
  for (unsigned i = 0, e = Intfs.size(); i != e; ++i) {
    LiveInterval &Spill = *Intfs[i];

    // Skip duplicates.
    if (!VRM->hasPhys(Spill.reg))
      continue;

    // Deallocate the interfering vreg by removing it from the union.
    // A LiveInterval instance may not be in a union during modification!
    Matrix->unassign(Spill);

    // Spill the extracted interval.
    LiveRangeEdit LRE(&Spill, SplitVRegs, *MF, *LIS, VRM);
    spiller().spill(LRE);
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                    Coloring-Based Coalescing Methods                       //
//===----------------------------------------------------------------------===//

void RAColorBasedCoalescing::algorithm(MachineFunction &mf) {
  srand(time(NULL));

  buildInterferenceGraph();

  calculateSpillCosts();

  simplify();

  biasedSelectExtended();

  // printInterferenceGraphWithColor();
}

void RAColorBasedCoalescing::calculateSpillCosts() {
  for(std::map<unsigned, std::set<unsigned>> :: iterator i = InterferenceGraph.begin(); i != InterferenceGraph.end(); i++) {
    double newSpillWeight = 0;
    unsigned vreg = i->first;

    // go over the def-use of virtual register
    for (MachineRegisterInfo::reg_instr_iterator I = MRI->reg_instr_begin(vreg), E = MRI->reg_instr_end(); I != E; ) {
      MachineInstr *machInst = &*(I++);
      unsigned loopDepth = MLI->getLoopDepth(machInst->getParent());

      if (loopDepth > 35) {
          loopDepth = 35; // Avoid overflowing the variable
      }

      std::pair<bool, bool> readWrite = machInst->readsWritesVirtualRegister(vreg);
      newSpillWeight += (readWrite.first + readWrite.second) * pow(10, loopDepth);
    }

    SpillWeight[vreg] = newSpillWeight;
  }
}

void RAColorBasedCoalescing::clearAll() {
  InterferenceGraph.clear();
  OnStack.clear();
  ColorsTemp.clear();
  Degree.clear();
  ExtendedColors.clear();
  SpillWeight.clear();
  CopyRelated.clear();
  Colors.clear();
}

bool RAColorBasedCoalescing::isMarkedForSpill(unsigned vreg) {
  return Colors[vreg] < 0;
}

// ===-------------- Interference Graph methods --------------===

void RAColorBasedCoalescing::buildInterferenceGraph() {
  for(unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {

    //reg ID
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if(MRI->reg_nodbg_empty(Reg)) {
      continue;
    }

    //get the respective LiveInterval
    LiveInterval *VirtReg = &LIS->getInterval(Reg);
    unsigned vReg = VirtReg->reg;

    // Ignores vReg if marked for spill
    if (isMarkedForSpill(vReg)) {
      continue;
    }

    OnStack[vReg] = false;

    InterferenceGraph[vReg].insert(0);
    InterferenceGraph[vReg].erase(0);

    //For each vReg1
    for(unsigned j = 0, r = MRI->getNumVirtRegs(); j != r; ++j) {
      unsigned Reg1 = TargetRegisterInfo::index2VirtReg(j);
      if(MRI->reg_nodbg_empty(Reg1)) {
          continue;
      }
      LiveInterval *VirtReg1 = &LIS->getInterval(Reg1);
      unsigned vReg1 = VirtReg1->reg;

      //ignores if equal
      if(VirtReg == VirtReg1) {
        continue;
      }

      if (isMarkedForSpill(vReg1)) {
        continue;
      }

      //if interference exists
      if(VirtReg->overlaps(*VirtReg1)) {

        // add edge vReg -> vReg1 on the interference graph
        if(!InterferenceGraph[vReg].count(vReg1)) {
          InterferenceGraph[vReg].insert(vReg1);
          Degree[vReg]++;
        }

        // add edge vReg1 -> vReg on the interference graph
        if(!InterferenceGraph[vReg1].count(vReg)) {
          InterferenceGraph[vReg1].insert(vReg);
          Degree[vReg1]++;
        }
      }
    }
  }
}

void RAColorBasedCoalescing::printInterferenceGraph() {
  dbgs() << " Interference Graph: \n";
  dbgs() << "-----------------------------------------------------------------\n";
  for(std::map<unsigned, std::set<unsigned>> :: iterator j = InterferenceGraph.begin(); j != InterferenceGraph.end(); j++) {
    dbgs() << "Interferences of " << j->first << "::" << PrintReg(j->first, TRI) << " => " << Degree[j->first] << ": {";
    for(std::set<unsigned> :: iterator k = j->second.begin(); k != j->second.end(); k++) {
      dbgs() << *k << ",";
    }
    dbgs() << "}\n";
  }
  dbgs() << "-----------------------------------------------------------------\n";
}

void RAColorBasedCoalescing::printInterferenceGraphWithColor() {
  dbgs() << " Interference Graph: \n";
  dbgs() << "-----------------------------------------------------------------\n";
  for(std::map<unsigned, std::set<unsigned>> :: iterator j = InterferenceGraph.begin(); j != InterferenceGraph.end(); j++) {
    dbgs() << "Interferences of " << j->first << "::" << PrintReg(j->first, TRI) << " => " << j->second.size() << ": {";
    for(std::set<unsigned> :: iterator k = j->second.begin(); k != j->second.end(); k++) {
      dbgs() << *k << ",";
    }
    dbgs() << "}";

    int color = ColorsTemp[j->first];
    if (isExtendedColor(color)) {
      dbgs() << " -- EXTENDED COLOR => " << color << "\n";
    } else {
      dbgs() << " -- COLOR => " << color << "::" << PrintReg(color, TRI) << "\n";
    }
  }
  dbgs() << "-----------------------------------------------------------------\n";
}

// ===-------------- Coloring methods --------------===

void RAColorBasedCoalescing::simplify() {
  // inserting virtual registers in the priority queue sorted by their degree
  for(std::map<unsigned, std::set<unsigned>> :: iterator i = InterferenceGraph.begin(); i != InterferenceGraph.end(); i++) {
    unsigned vreg = i->first;
    ColoringPq.push(std::pair<unsigned, unsigned>(Degree[vreg], vreg));
  }
}

void RAColorBasedCoalescing::biasedSelectExtended() {
  while(!ColoringPq.empty()) {
    unsigned vreg = ColoringPq.top().second;
    ColoringPq.pop();

    std::vector<int> potentialRegs = getPotentialRegs(vreg);

    int color = getColor(potentialRegs, vreg);

    if (color == COLOR_INVALID) {
      color = getColor(ExtendedColors, vreg);

      if (color == COLOR_INVALID) {
        color = createNewExtendedColor();
      }
    }

    ColorsTemp[vreg] = color;
  }
}

std::vector<int> RAColorBasedCoalescing::getPotentialRegs(unsigned vreg) {
  std::vector<int> potentialRegs;

  AllocationOrder Order(vreg, *VRM, RegClassInfo, Matrix);
  while (unsigned physReg = Order.next()) {
    potentialRegs.push_back(physReg);
  }

  sort(potentialRegs.begin(), potentialRegs.end());
  return potentialRegs;
}

int RAColorBasedCoalescing::getColor(std::vector<int> Colors, unsigned vreg) {

  std::set<int> neighborColors;

  // inserting the color of the neighbors of vreg in a set
  for(std::set<unsigned> :: iterator j = InterferenceGraph[vreg].begin(); j != InterferenceGraph[vreg].end(); j++) {
    int colorOfNeighbor = ColorsTemp[*j]; // returns 0 if ColorsTemp[*j] doesn't exist

    if(colorOfNeighbor != 0)
      neighborColors.insert(colorOfNeighbor);
  }

  // calculating the number of possible colors to select to vreg
  unsigned possibilities = Colors.size() - neighborColors.size();
  if(possibilities == 0)
    return COLOR_INVALID;

  // assing a random number to k from 0 to (possibilities - 1)
  unsigned k = rand() % possibilities;

  // selecting the k-th color to vreg
  for(int color: Colors) {
    
    // if color already belongs to a neighbor, we can ignore it
    if(neighborColors.count(color))
      continue;

    if(k == 0)
      return color;

    k--;
  }

  return COLOR_INVALID;
}

int RAColorBasedCoalescing::createNewExtendedColor() {
  int new_color;
  if (ExtendedColors.empty()) {
    new_color = -1;
  } else {
    new_color = ExtendedColors.back() - 1;
  }

  ExtendedColors.push_back(new_color);

  return new_color;
}

bool RAColorBasedCoalescing::isExtendedColor(int color) {
  return color < 0;
}

// ===-------------- LLVM --------------===

bool RAColorBasedCoalescing::runOnMachineFunction(MachineFunction &mf) {
  /*dbgs() << "\n********** COLORING-BASED COALESCING REGISTER ALLOCATION **********\n"
              << "********** Function: "
              << mf.getName() << '\n';*/

  MF = &mf;
  RegAllocBase::init(getAnalysis<VirtRegMap>(),
                     getAnalysis<LiveIntervals>(),
                     getAnalysis<LiveRegMatrix>());

  MBFI = &getAnalysis<MachineBlockFrequencyInfo>();
  DomTree = &getAnalysis<MachineDominatorTree>();


  calculateSpillWeightsAndHints(*LIS, *MF, VRM,
                                getAnalysis<MachineLoopInfo>(),
                                getAnalysis<MachineBlockFrequencyInfo>());

  SpillerInstance.reset(createInlineSpiller(*this, *MF, *VRM));

  MLI = &getAnalysis<MachineLoopInfo>();
  DebugVars = &getAnalysis<LiveDebugVariables>();
  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();


  // dbgs() << "********** Number of virtual registers: " << MRI->getNumVirtRegs() << "\n\n";

  // printVirtualRegisters();

  algorithm(mf);

  allocatePhysRegs();
  postOptimization();

  clearAll();

  // Diagnostic output before rewriting
  // dbgs() << "\nPost alloc VirtRegMap:\n" << *VRM << "\n";

  releaseMemory();
  return true;
}

void RAColorBasedCoalescing::printVirtualRegisters() {
  dbgs() << " Virtual Registers: \n";
  dbgs() << "-----------------------------------------------------------------\n";
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    LiveInterval *VirtReg = &LIS->getInterval(Reg);
    dbgs() << *VirtReg << "::" << Reg <<'\n';
  }
  dbgs() << "-----------------------------------------------------------------\n\n";
}

FunctionPass *llvm::createColorBasedRegAlloc() {
  return new RAColorBasedCoalescing();
}