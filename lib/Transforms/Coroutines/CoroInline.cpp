//===- CoroInline.cpp - Coroutine Inline Pass -----------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// CoroInline - A wrapper pass around regular inliner pass
//
//===----------------------------------------------------------------------===//

#include "CoroutineCommon.h"
#include "CoroSplit4.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Transforms/Scalar.h"

using namespace llvm;
using namespace llvm::coro;

#define DEBUG_TYPE "coro-inline"

namespace {
  /// PrintCallGraphPass - Print a Module corresponding to a call graph.
  ///
  class CoroInline : public CallGraphSCCPass, CoroutineCommon {
  public:
    static char ID;
    CoroInline()
      : CallGraphSCCPass(ID) {
      initializeCoroInlinePass(*PassRegistry::getPassRegistry());
    }

    SmallSetVector<Function*, 8> Coroutines;
    SmallVector<CoroutineData, 8> CoroData;

    void AddCoroutine(CallGraph& CG, Function& F) {
      if (Coroutines.insert(&F)) {
        CoroData.emplace_back(F);
        auto rf = CG.getOrInsertFunction(CoroData.back().Resume.Func);
        CG.getExternalCallingNode()->addCalledFunction(CallSite(), rf);
        auto cf = CG.getOrInsertFunction(CoroData.back().Cleanup.Func);
        CG.getExternalCallingNode()->addCalledFunction(CallSite(), cf);
        auto df = CG.getOrInsertFunction(CoroData.back().Destroy.Func);
        CG.getExternalCallingNode()->addCalledFunction(CallSite(), df);
      }
    }

    bool HasCoroInit;

    void inlineResumeCleanup(Function& F) {
      SmallVector<CallSite, 8> CoroCalls;
      for (auto &I : instructions(F))
        if (CallSite CS = CallSite(&I))
          if (auto Callee = CS.getCalledFunction())
            if (Callee->getName().endswith(".cleanup") ||
                Callee->getName().endswith(".resume") ||
                Callee->getName().endswith(".destroy"))
              CoroCalls.push_back(CS);

      for (auto& CS : CoroCalls) {
        // TODO: get proper inline threshold Inliner::getInlineThreshold(CS))
        if (ICA->getInlineCost(CS, 100)) {
          InlineFunctionInfo IFI;
          InlineFunction(CS, IFI);
        }
      }
    }

    bool tryCoroElide(CallGraphSCC &SCC) {
      bool changed = false;
      for (CallGraphNode *Node : SCC) {
        Function *F = Node->getFunction();
        if (F) {
          auto CI = FindIntrinsic(*F, Intrinsic::coro_init);
          if (!CI) return false;
          auto CD = FindIntrinsic(*F, Intrinsic::coro_destroy);
          if (!CD) return false;

          legacy::FunctionPassManager FPM(F->getParent());
          FPM.add(createSROAPass());
          FPM.add(createInstructionCombiningPass());  // Combine silly seq's
          FPM.add(createCoroHeapElidePass());

     //     FPM.add(createSROAPass());
     //     FPM.add(createEarlyCSEPass());              // Catch trivial redundancies
#if 1
          FPM.add(createEarlyCSEPass());              // Catch trivial redundancies
          FPM.add(createJumpThreadingPass());         // Thread jumps.
          FPM.add(createCorrelatedValuePropagationPass()); // Propagate conditionals
          FPM.add(createCFGSimplificationPass());     // Merge & remove BBs
          FPM.add(createInstructionCombiningPass());  // Combine silly seq's
          //FPM.add(createTailCallEliminationPass()); // Eliminate tail calls
          //FPM.add(createCFGSimplificationPass());     // Merge & remove BBs
          //FPM.add(createReassociatePass());           // Reassociate expressions
                                                      // Rotate Loop - disable header duplication at -Oz
          //FPM.add(createLoopRotatePass());

          //FPM.add(createSROAPass());
          //FPM.add(createEarlyCSEPass());
          //FPM.add(createJumpThreadingPass());
          //FPM.add(createCFGSimplificationPass());
          //FPM.add(createInstructionCombiningPass());
          //FPM.add(createGVNPass());
          //FPM.add(createCorrelatedValuePropagationPass());
#endif
          FPM.doInitialization();
          FPM.run(*F);
          FPM.doFinalization();
          inlineResumeCleanup(*F);
          RefreshCallGraph(SCC, *CurrentCG, false);
          changed = true;
        }
      }
      return changed;
    }

    /// Scan the functions in the specified CFG and resync the
    /// callgraph with the call sites found in it.  This is used after
    /// FunctionPasses have potentially munged the callgraph, and can be used after
    /// CallGraphSCC passes to verify that they correctly updated the callgraph.
    ///
    /// This function returns true if it devirtualized an existing function call,
    /// meaning it turned an indirect call into a direct call.  This happens when
    /// a function pass like GVN optimizes away stuff feeding the indirect call.
    /// This never happens in checking mode.
    ///
    bool RefreshCallGraph(CallGraphSCC &CurSCC,
      CallGraph &CG, bool CheckingMode) {
      DenseMap<Value*, CallGraphNode*> CallSites;

      DEBUG(dbgs() << "CGSCCPASSMGR: Refreshing SCC with " << CurSCC.size()
        << " nodes:\n";
      for (CallGraphNode *CGN : CurSCC)
        CGN->dump();
      );

      bool MadeChange = false;
      bool DevirtualizedCall = false;

      // Scan all functions in the SCC.
      unsigned FunctionNo = 0;
      for (CallGraphSCC::iterator SCCIdx = CurSCC.begin(), E = CurSCC.end();
      SCCIdx != E; ++SCCIdx, ++FunctionNo) {
        CallGraphNode *CGN = *SCCIdx;
        Function *F = CGN->getFunction();
        if (!F || F->isDeclaration()) continue;

        // Walk the function body looking for call sites.  Sync up the call sites in
        // CGN with those actually in the function.

        // Keep track of the number of direct and indirect calls that were
        // invalidated and removed.
        unsigned NumDirectRemoved = 0, NumIndirectRemoved = 0;

        // Get the set of call sites currently in the function.
        for (CallGraphNode::iterator I = CGN->begin(), E = CGN->end(); I != E; ) {
          // If this call site is null, then the function pass deleted the call
          // entirely and the WeakVH nulled it out.  
          if (!I->first ||
            // If we've already seen this call site, then the FunctionPass RAUW'd
            // one call with another, which resulted in two "uses" in the edge
            // list of the same call.
            CallSites.count(I->first) ||

            // If the call edge is not from a call or invoke, or it is a
            // instrinsic call, then the function pass RAUW'd a call with 
            // another value. This can happen when constant folding happens
            // of well known functions etc.
            !CallSite(I->first) ||
            (CallSite(I->first).getCalledFunction() &&
              CallSite(I->first).getCalledFunction()->isIntrinsic() &&
              Intrinsic::isLeaf(
                CallSite(I->first).getCalledFunction()->getIntrinsicID()))) {
            assert(!CheckingMode &&
              "CallGraphSCCPass did not update the CallGraph correctly!");

            // If this was an indirect call site, count it.
            if (!I->second->getFunction())
              ++NumIndirectRemoved;
            else
              ++NumDirectRemoved;

            // Just remove the edge from the set of callees, keep track of whether
            // I points to the last element of the vector.
            bool WasLast = I + 1 == E;
            CGN->removeCallEdge(I);

            // If I pointed to the last element of the vector, we have to bail out:
            // iterator checking rejects comparisons of the resultant pointer with
            // end.
            if (WasLast)
              break;
            E = CGN->end();
            continue;
          }

          assert(!CallSites.count(I->first) &&
            "Call site occurs in node multiple times");

          CallSite CS(I->first);
          if (CS) {
            Function *Callee = CS.getCalledFunction();
            // Ignore intrinsics because they're not really function calls.
            if (!Callee || !(Callee->isIntrinsic()))
              CallSites.insert(std::make_pair(I->first, I->second));
          }
          ++I;
        }

        // Loop over all of the instructions in the function, getting the callsites.
        // Keep track of the number of direct/indirect calls added.
        unsigned NumDirectAdded = 0, NumIndirectAdded = 0;

        for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB)
          for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {
            CallSite CS(cast<Value>(I));
            if (!CS) continue;
            Function *Callee = CS.getCalledFunction();
            if (Callee && Callee->isIntrinsic()) continue;

            // If this call site already existed in the callgraph, just verify it
            // matches up to expectations and remove it from CallSites.
            DenseMap<Value*, CallGraphNode*>::iterator ExistingIt =
              CallSites.find(CS.getInstruction());
            if (ExistingIt != CallSites.end()) {
              CallGraphNode *ExistingNode = ExistingIt->second;

              // Remove from CallSites since we have now seen it.
              CallSites.erase(ExistingIt);

              // Verify that the callee is right.
              if (ExistingNode->getFunction() == CS.getCalledFunction())
                continue;

              // If we are in checking mode, we are not allowed to actually mutate
              // the callgraph.  If this is a case where we can infer that the
              // callgraph is less precise than it could be (e.g. an indirect call
              // site could be turned direct), don't reject it in checking mode, and
              // don't tweak it to be more precise.
              if (CheckingMode && CS.getCalledFunction() &&
                ExistingNode->getFunction() == nullptr)
                continue;

              assert(!CheckingMode &&
                "CallGraphSCCPass did not update the CallGraph correctly!");

              // If not, we either went from a direct call to indirect, indirect to
              // direct, or direct to different direct.
              CallGraphNode *CalleeNode;
              if (Function *Callee = CS.getCalledFunction()) {
                CalleeNode = CG.getOrInsertFunction(Callee);
                // Keep track of whether we turned an indirect call into a direct
                // one.
                if (!ExistingNode->getFunction()) {
                  DevirtualizedCall = true;
                  DEBUG(dbgs() << "  CGSCCPASSMGR: Devirtualized call to '"
                    << Callee->getName() << "'\n");
                }
              }
              else {
                CalleeNode = CG.getCallsExternalNode();
              }

              // Update the edge target in CGN.
              CGN->replaceCallEdge(CS, CS, CalleeNode);
              MadeChange = true;
              continue;
            }

            assert(!CheckingMode &&
              "CallGraphSCCPass did not update the CallGraph correctly!");

            // If the call site didn't exist in the CGN yet, add it.
            CallGraphNode *CalleeNode;
            if (Function *Callee = CS.getCalledFunction()) {
              CalleeNode = CG.getOrInsertFunction(Callee);
              ++NumDirectAdded;
            }
            else {
              CalleeNode = CG.getCallsExternalNode();
              ++NumIndirectAdded;
            }

            CGN->addCalledFunction(CS, CalleeNode);
            MadeChange = true;
          }

        // We scanned the old callgraph node, removing invalidated call sites and
        // then added back newly found call sites.  One thing that can happen is
        // that an old indirect call site was deleted and replaced with a new direct
        // call.  In this case, we have devirtualized a call, and CGSCCPM would like
        // to iteratively optimize the new code.  Unfortunately, we don't really
        // have a great way to detect when this happens.  As an approximation, we
        // just look at whether the number of indirect calls is reduced and the
        // number of direct calls is increased.  There are tons of ways to fool this
        // (e.g. DCE'ing an indirect call and duplicating an unrelated block with a
        // direct call) but this is close enough.
        if (NumIndirectRemoved > NumIndirectAdded &&
          NumDirectRemoved < NumDirectAdded)
          DevirtualizedCall = true;

        // After scanning this function, if we still have entries in callsites, then
        // they are dangling pointers.  WeakVH should save us for this, so abort if
        // this happens.
        assert(CallSites.empty() && "Dangling pointers found in call sites map");

        // Periodically do an explicit clear to remove tombstones when processing
        // large scc's.
        if ((FunctionNo & 15) == 15)
          CallSites.clear();
      }

      DEBUG(if (MadeChange) {
        dbgs() << "CGSCCPASSMGR: Refreshed SCC is now:\n";
        for (CallGraphNode *CGN : CurSCC)
          CGN->dump();
        if (DevirtualizedCall)
          dbgs() << "CGSCCPASSMGR: Refresh devirtualized a call!\n";

      }
      else {
        dbgs() << "CGSCCPASSMGR: SCC Refresh didn't change call graph.\n";
      }
      );
      (void)MadeChange;

      return DevirtualizedCall;
    }

    void addToCallGraph(Function *F) {
      if (!F)
        return;

      CallGraph& CG = *CurrentCG;
      CallGraphNode *Node = CG.getOrInsertFunction(F);

      // Look for calls by this function.
      for (Function::iterator BB = F->begin(), BBE = F->end(); BB != BBE; ++BB)
        for (BasicBlock::iterator II = BB->begin(), IE = BB->end(); II != IE;
      ++II) {
        CallSite CS(cast<Value>(II));
        if (CS) {
          const Function *Callee = CS.getCalledFunction();
          if (!Callee || !Intrinsic::isLeaf(Callee->getIntrinsicID()))
            // Indirect calls of intrinsics are not allowed so no need to check.
            // We can be more precise here by using TargetArg returned by
            // Intrinsic::isLeaf.
            Node->addCalledFunction(CS, CG.getCallsExternalNode());
          else if (!Callee->isIntrinsic())
            Node->addCalledFunction(CS, CG.getOrInsertFunction(Callee));
        }
      }
    }

    InlineCostAnalysis *ICA;
    bool runOnSCC(CallGraphSCC &SCC) override {
      bool changed = false;
      ICA = &getAnalysis<InlineCostAnalysis>();

      if (HasCoroInit)
        changed |= tryCoroElide(SCC);

      SmallPtrSet<Function*, 8> SCCFunctions;
      //DEBUG(dbgs() << "Inliner visiting SCC:");
      for (CallGraphNode *Node : SCC) {
        Function *F = Node->getFunction();
        if (F) {
          auto FI = std::find(Coroutines.begin(), Coroutines.end(), F);
          if (FI != Coroutines.end()) {
            auto& CD = CoroData[FI - Coroutines.begin()];
            CD.split(this);
            RefreshCallGraph(SCC, *CurrentCG, false);
            addToCallGraph(CD.Resume.Func);
            addToCallGraph(CD.Cleanup.Func);
            addToCallGraph(CD.Destroy.Func);
            changed = true;
          }
        }
      }

      return changed; // Inliner->runOnSCC(SCC);
    }

    CallGraph* CurrentCG;

    bool doInitialization(CallGraph &CG) override {
      CurrentCG = &CG;
      Module& M = CG.getModule();
      CoroutineCommon::PerModuleInit(M);
      auto CoroSuspend = Intrinsic::getDeclaration(&M, Intrinsic::coro_suspend2);
      auto CoroInit = Intrinsic::getDeclaration(&M, Intrinsic::coro_init);
      HasCoroInit = !CoroInit->user_empty();
      for (User* U : CoroSuspend->users())
        if (auto* I = dyn_cast<Instruction>(U))
          AddCoroutine(CG, *I->getParent()->getParent());
      return !Coroutines.empty();
    }

    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<InlineCostAnalysis>();
      //Inliner::getAnalysisUsage(AU);
    }

    //bool doFinalization(CallGraph &CG) override {
    //  return Inliner->doFinalization(CG);
    //}

  };

} // end anonymous namespace.

char CoroInline::ID = 0;
INITIALIZE_PASS_BEGIN(CoroInline, "coro-inline",
  "Coroutine Integration/Inlining", false, false)
  INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
  INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(InlineCostAnalysis)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_END(CoroInline, "coro-inline",
    "Coroutine Integration/Inlining", false, false)

// TODO: add pass dependency coro-split
Pass *llvm::createCoroInline() { 
  return new CoroInline(); 
}
