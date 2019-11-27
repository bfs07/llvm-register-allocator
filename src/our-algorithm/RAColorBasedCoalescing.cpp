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

#include <iostream>
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
#include <algorithm>
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
  std::stack<unsigned> ColoringStack;
  std::map<unsigned, int> ColorsTemp;
  std::map<unsigned, int> Colors;
  std::map<unsigned, std::set<unsigned>> CopyRelated;
  std::list<int> ExtendedColors;
  std::map<unsigned, double> SpillWeight;
  // the key represents the id associated to the real color (0 ... maxColor) and the real color.
  std::map<unsigned, std::pair<unsigned, unsigned>> color;


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

      void clear();

      void clearAll();

      bool spillCode();

      void save();

      bool spillInterferences(LiveInterval &VirtReg, unsigned PhysReg, SmallVectorImpl<unsigned> &SplitVRegs);

      unsigned selectOrSplit1(LiveInterval &VirtReg, SmallVectorImpl<unsigned> &SplitVRegs);

      bool isMarkedForSpill(unsigned vreg);

      void assignColor(unsigned VirtRegID);

      // ===-------------- Interference Graph methods --------------===

      void buildInterferenceGraph();

      void printInterferenceGraph();

      void printInterferenceGraphWithColor();

      // ===-------------- Coloring methods --------------===

      void simplify();

      int getNumPhysicalRegs(unsigned VirtRegID);

      void biasedSelectExtended();

      bool isExtendedColor(int color);

      std::list<int> getPotentialRegs(unsigned vreg);

      int getColor(std::list<int> Colors, unsigned vreg);

      int createNewExtendedColor();

      // ===-------------- Coalescing --------------===

      void coalescing();

      void coalesce(unsigned copy_related1, unsigned copy_related2);


      // ===-------------- Spliting --------------===

      void trySplitAll();

      unsigned trySplit(LiveInterval &VirtReg, AllocationOrder &Order, SmallVectorImpl<unsigned> &NewVRegs);

      void printVirtualRegisters();


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
  
  // if it is not marked for spill
  if(color[VirtReg.reg].first != 30)
    return color[VirtReg.reg].second;  

  if (!VirtReg.isSpillable())
    return ~0u;
  
  //dbgs() << "SPILLING: " << PrintReg(VirtReg.reg, TRI);
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, nullptr, &DeadRemats);
  spiller().spill(LRE);
  
  return 0;
}

//===----------------------------------------------------------------------===//
//                    Coloring-Based Coalescing Methods                       //
//===----------------------------------------------------------------------===//

void RAColorBasedCoalescing::algorithm(MachineFunction &mf) {

    buildInterferenceGraph();

    simplify();

    //printInterferenceGraphWithColor();

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

void RAColorBasedCoalescing::printInterferenceGraphWithColor() {
  dbgs() << " Interference Graph: \n";
  dbgs() << "-----------------------------------------------------------------\n";
  for(std::map<unsigned, std::set<unsigned>> :: iterator j = InterferenceGraph.begin(); j != InterferenceGraph.end(); j++) {
    dbgs() << "Interferences of " << j->first << "::" << PrintReg(j->first, TRI) << " => " << j->second.size() << ": {";
    for(std::set<unsigned> :: iterator k = j->second.begin(); k != j->second.end(); k++) {
      dbgs() << *k << ",";
    }
    dbgs() << "}";
    if(color[j->first].first != 30)
      dbgs() << " -- COLOR => " << color[j->first].first << "::" << PrintReg(color[j->first].second, TRI) << "\n";
    else 
      dbgs() << " -- SPILLED" << "\n";
    
  }
  dbgs() << "-----------------------------------------------------------------\n";
}

// ===-------------- Coloring methods --------------===

/// Verifies if the k-th bit is set.
bool isSet(int number, int k) {
  return (number >> k) & 1; 
}

int RAColorBasedCoalescing::getNumPhysicalRegs(unsigned VirtRegID) {

  AllocationOrder Order(VirtRegID, *VRM, RegClassInfo, Matrix);

  int cnt = 0;
  while (Order.next()) {
    cnt++;
  }

  return cnt;

}

void RAColorBasedCoalescing::assignColor(unsigned VirtRegID) {

  // Check for an available register in this class.
  AllocationOrder Order(VirtRegID, *VRM, RegClassInfo, Matrix);

  std::vector<unsigned> pr;
  while (unsigned PhysReg = Order.next()) {
    pr.push_back(PhysReg);
  }

  std::sort(pr.begin(), pr.end());
  color[VirtRegID].second = pr[color[VirtRegID].first];
} 

void RAColorBasedCoalescing::simplify() {

  srand(time(nullptr));

  std::priority_queue<std::pair<unsigned, unsigned>> pq;

  for(unsigned j = 0, r = MRI->getNumVirtRegs(); j != r; ++j) {
    unsigned reg = TargetRegisterInfo::index2VirtReg(j);

    // if is not a DEBUG register
    if (MRI->reg_nodbg_empty(reg))
      continue;

    pq.push(std::pair<unsigned, unsigned>(Degree[reg], reg));
  }

  while(!pq.empty()) {

    unsigned u = pq.top().second;
    pq.pop();
    const int maxColor = getNumPhysicalRegs(u);

    unsigned mask = 0;
    for(auto v: InterferenceGraph[u])
      if(color[u].first != 30u && color.count(v))
        // If a neighbor is already colored with valid color c, the c-th bit is set.
        mask |= (1 << color[v].first);

    /// Counts the amount of non-active bits in a number.
    int cnt = 0;
    for(int i = 0; i < maxColor; i++)
      if(!isSet(mask, i))
        cnt++;

    // If the number of different colors of the neighbors of u is equal to
    // zero (there isn't any Physical register available), this virtual register
    // should be spilled (represented by the color 30).
    if(cnt == 0) {
      color[u] = std::make_pair(30u, 0u);
    } else {

      int k = rand() % cnt;

      /// Assigns the k-th color chosen randomly to the register.
      for(int i = 0; i < maxColor; i++) {
        // If the k-th was reached, assign the i-th color to the register. 
        if(k == 0)
          color[u] = std::make_pair(i, 0);

        if(!isSet(mask, i))
          k--;
      } 
    }

    assignColor(u);

  }
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


  //dbgs() << "********** Number of virtual registers: " << MRI->getNumVirtRegs() << "\n\n";


  //printVirtualRegisters();

  algorithm(mf);

  allocatePhysRegs();
  postOptimization();

  clearAll();

  // Diagnostic output before rewriting
  //dbgs() << "\nPost alloc VirtRegMap:\n" << *VRM << "\n";

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
