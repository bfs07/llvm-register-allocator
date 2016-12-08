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
// a implementation of the Color-based coalescing register allocator.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "AllocationOrder.h"
#include "LiveDebugVariables.h"
#include "RegAllocBase.h"
#include "Spiller.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStackAnalysis.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
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
#include <queue>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

namespace llvm {
  FunctionPass *createMyRegAlloc();
}
static RegisterRegAlloc colorBasedCoalescingRegAlloc("myregalloc",
                                                     "color-based coalescing register allocator",
                                                     createMyRegAlloc);


namespace {
  struct CompSpillWeight {
    bool operator()(LiveInterval *A, LiveInterval *B) const {
      return A->weight < B->weight;
    }
  };
}

namespace {

  std::map<unsigned, std::set<unsigned>> InterferenceGraph;
  std::map<unsigned, int> Degree;
/// RAColorBasedCoalescing provides a minimal implementation of the basic register allocation
/// algorithm. It prioritizes live virtual registers by spill weight and spills
/// whenever a register is unavailable. This is not practical in production but
/// provides a useful baseline both for measuring other allocators and comparing
/// the speed of the basic algorithm against other styles of allocators.
class RAColorBasedCoalescing : public MachineFunctionPass, public RegAllocBase
{
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
  void buildInterferenceGraph();

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

  unsigned selectOrSplit(LiveInterval &VirtReg,
                         SmallVectorImpl<unsigned> &SplitVRegs) override;

  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  // Helper for spilling all live virtual registers currently unified under preg
  // that interfere with the most recently queried lvr.  Return true if spilling
  // was successful, and append any new spilled/split intervals to splitLVRs.
  bool spillInterferences(LiveInterval &VirtReg, unsigned PhysReg,
                          SmallVectorImpl<unsigned> &SplitVRegs);

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


// Spill or split all live virtual registers currently unified under PhysReg
// that interfere with VirtReg. The newly spilled or split live intervals are
// returned by appending them to SplitVRegs.
bool RAColorBasedCoalescing::spillInterferences(LiveInterval &VirtReg, unsigned PhysReg,
                                 SmallVectorImpl<unsigned> &SplitVRegs) {
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
  DEBUG(dbgs() << "spilling " << TRI->getName(PhysReg) <<
        " interferences with " << VirtReg << "\n");
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
    LiveRangeEdit LRE(&Spill, SplitVRegs, *MF, *LIS, VRM, nullptr, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}

// Driver for the register assignment and splitting heuristics.
// Manages iteration over the LiveIntervalUnions.
//
// This is a minimal implementation of register assignment and splitting that
// spills whenever we run out of registers.
//
// selectOrSplit can only be called once per live virtual register. We then do a
// single interference test for each register the correct class until we find an
// available register. So, the number of interference tests in the worst case is
// |vregs| * |machineregs|. And since the number of interference tests is
// minimal, there is no value in caching them outside the scope of
// selectOrSplit().
unsigned RAColorBasedCoalescing::selectOrSplit(LiveInterval &VirtReg,
                                SmallVectorImpl<unsigned> &SplitVRegs) {
  // Populate a list of physical register spill candidates.
  SmallVector<unsigned, 8> PhysRegSpillCands;

  // Check for an available register in this class.
  AllocationOrder Order(VirtReg.reg, *VRM, RegClassInfo, Matrix);
  while (unsigned PhysReg = Order.next()) {
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
  DEBUG(dbgs() << "spilling: " << VirtReg << '\n');
  if (!VirtReg.isSpillable())
    return ~0u;
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, nullptr, &DeadRemats);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  return 0;
}

bool RAColorBasedCoalescing::runOnMachineFunction(MachineFunction &mf) {
  DEBUG(dbgs() << "********** COLOR-BASED COALESCING REGISTER ALLOCATION **********\n"
               << "********** Function: "
               << mf.getName() << '\n');

  dbgs() << "********** COLOR-BASED COALESCING REGISTER ALLOCATION **********\n"
               << "********** Function: "
               << mf.getName() << '\n';

  MF = &mf;
  RegAllocBase::init(getAnalysis<VirtRegMap>(),
                     getAnalysis<LiveIntervals>(),
                     getAnalysis<LiveRegMatrix>());
  buildInterferenceGraph();
  calculateSpillWeightsAndHints(*LIS, *MF, VRM,
                                getAnalysis<MachineLoopInfo>(),
                                getAnalysis<MachineBlockFrequencyInfo>());

  SpillerInstance.reset(createInlineSpiller(*this, *MF, *VRM));

  allocatePhysRegs();
  postOptimization();

  // Diagnostic output before rewriting
  DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  return true;
}

//Builds Interference Graph
void RAColorBasedCoalescing::buildInterferenceGraph()
{
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    // reg ID
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    dbgs() << "index2VirtReg i: " << i << " Reg: " << Reg << "\n";
    dbgs() << PrintReg(Reg, TRI) << "\n";
    // if is not a DEBUG register
    if (MRI->reg_nodbg_empty(Reg)) {
      dbgs() << "It is a DEBUG register.\n";
      continue;
    }
    // get the respective LiveInterval
    LiveInterval *VirtReg = &LIS->getInterval(Reg);
    dbgs() << "LiveInterval: " << VirtReg << "\n";
  }
	// int num=0;
	// for (LiveIntervals::iterator ii = LIS->begin(); ii != LIS->end(); ii++)
	// {
  //
	// 	if(TRI->isPhysicalRegister(ii->first))
	// 		continue;
	// 	num++;
	// 	OnStack[ii->first] = false;
	// 	InterferenceGraph[ii->first].insert(0);
	// 	const LiveInterval *li = ii->second;
	// 	for (LiveIntervals::iterator jj = LIS->begin(); jj != LIS->end(); jj++)
	// 	{
	// 		const LiveInterval *li2 = jj->second;
	// 		if(jj->first == ii->first)
	// 			continue;
	// 		if(TRI->isPhysicalRegister(jj->first))
	// 			continue;
	// 		if (li->overlaps(*li2))
	// 		{
	// 			if(!InterferenceGraph[ii->first].count(jj->first))
	// 			{
	// 				InterferenceGraph[ii->first].insert(jj->first);
	// 				Degree[ii->first]++;
	// 			}
	// 			if(!InterferenceGraph[jj->first].count(ii->first))
	// 			{
	// 				InterferenceGraph[jj->first].insert(ii->first);
	// 				Degree[jj->first]++;
	// 			}
	// 		}
	// 	}
	// }
	// dbgs( )<<"\nVirtual registers: "<<num;
}

FunctionPass *llvm::createMyRegAlloc()
{
  return new RAColorBasedCoalescing();
}