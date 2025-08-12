#include "SVF-LLVM/LeopardMetrics.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Type.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopInfoImpl.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Support/GenericDomTree.h"
#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <queue>

using namespace llvm;
using namespace SVF;

namespace {

std::string getFunctionName(const llvm::Function* F) {
    if (!F) return "<null>";
    if (F->hasName()) return F->getName().str();
    return "<unnamed>";
}

// Helper: assign index to basic blocks in function order.
std::unordered_map<const BasicBlock*, unsigned>
getBasicBlockIndices(const Function& F) {
    std::unordered_map<const BasicBlock*, unsigned> result;
    unsigned idx = 0;
    for (const BasicBlock& BB : F)
        result[&BB] = idx++;
    return result;
}

// Helper: walk up dom tree, count number of conditional/switch/split blocks.
unsigned computeNestingLevel(
    const BasicBlock* BB,
    const DominatorTree* DT,
    const std::unordered_set<const BasicBlock*>& controlBlocks)
{
    unsigned level = 0;
    if (!DT || !BB) return 0;
    const BasicBlock* curr = BB;
    while (const DomTreeNodeBase<BasicBlock>* node = DT->getNode(curr)) {
        curr = node->getIDom() ? node->getIDom()->getBlock() : nullptr;
        if (curr && controlBlocks.count(curr)) ++level;
    }
    return level;
}

// Helper: recursively compute maximum loop nesting depth.
unsigned getMaxLoopNesting(const Loop* L) {
    if (!L) return 0;
    unsigned maxSub = 0;
    for (const Loop* sub : L->getSubLoops()) {
        unsigned subDepth = getMaxLoopNesting(sub);
        if (subDepth > maxSub) maxSub = subDepth;
    }
    return 1 + maxSub;
}

// Collect all loops in function.
void collectLoops(LoopInfo& LI, std::vector<const Loop*>& out) {
    for (auto* L : LI) {
        out.push_back(L);
        std::stack<const Loop*> work;
        for (auto* sub : L->getSubLoops()) work.push(sub);
        while (!work.empty()) {
            auto* sub = work.top(); work.pop();
            out.push_back(sub);
            for (auto* subsub : sub->getSubLoops()) work.push(subsub);
        }
    }
}

// Helper: collect pointer arith variables, histogram.
void analyzePointerOps(
    const Function& F,
    unsigned& ptrArithOps,
    std::unordered_map<const Value*, unsigned>& varHistogram)
{
    ptrArithOps = 0;
    for (const Instruction& I : instructions(F)) {
        if (isa<GetElementPtrInst>(&I) ||
            isa<PtrToIntInst>(&I) ||
            isa<IntToPtrInst>(&I) ||
            (isa<BitCastInst>(&I) &&
                (I.getOperand(0)->getType()->isPointerTy() || I.getType()->isPointerTy())) ||
            (isa<AddrSpaceCastInst>(&I) &&
                (I.getOperand(0)->getType()->isPointerTy() || I.getType()->isPointerTy())))
        {
            ++ptrArithOps;
            for (const Use& op : I.operands()) {
                if (op->getType()->isPointerTy()) varHistogram[op.get()]++;
            }
        }
        // ConstantExpr pointer arith via operands
        for (const Use& op : I.operands()) {
            if (const ConstantExpr* ce = dyn_cast<ConstantExpr>(op)) {
                if (ce->getOpcode() == Instruction::GetElementPtr ||
                    ce->getOpcode() == Instruction::PtrToInt ||
                    ce->getOpcode() == Instruction::IntToPtr ||
                    ce->getOpcode() == Instruction::BitCast ||
                    ce->getOpcode() == Instruction::AddrSpaceCast)
                {
                    ++ptrArithOps;
                    for (unsigned oi = 0, oe = ce->getNumOperands(); oi < oe; ++oi)
                        if (ce->getOperand(oi)->getType()->isPointerTy())
                            varHistogram[ce->getOperand(oi)]++;
                }
            }
        }
    }
}

void analyzePointerOpsBB(
    const BasicBlock& BB,
    unsigned& ptrArithOps,
    std::unordered_map<const Value*, unsigned>& varHistogram)
{
    ptrArithOps = 0;
    for (const Instruction& I : BB) {
        if (isa<GetElementPtrInst>(&I) ||
            isa<PtrToIntInst>(&I) ||
            isa<IntToPtrInst>(&I) ||
            (isa<BitCastInst>(&I) &&
                (I.getOperand(0)->getType()->isPointerTy() || I.getType()->isPointerTy())) ||
            (isa<AddrSpaceCastInst>(&I) &&
                (I.getOperand(0)->getType()->isPointerTy() || I.getType()->isPointerTy())))
        {
            ++ptrArithOps;
            for (const Use& op : I.operands()) {
                if (op->getType()->isPointerTy()) varHistogram[op.get()]++;
            }
        }
        for (const Use& op : I.operands()) {
            if (const ConstantExpr* ce = dyn_cast<ConstantExpr>(op)) {
                if (ce->getOpcode() == Instruction::GetElementPtr ||
                    ce->getOpcode() == Instruction::PtrToInt ||
                    ce->getOpcode() == Instruction::IntToPtr ||
                    ce->getOpcode() == Instruction::BitCast ||
                    ce->getOpcode() == Instruction::AddrSpaceCast)
                {
                    ++ptrArithOps;
                    for (unsigned oi = 0, oe = ce->getNumOperands(); oi < oe; ++oi)
                        if (ce->getOperand(oi)->getType()->isPointerTy())
                            varHistogram[ce->getOperand(oi)]++;
                }
            }
        }
    }
}

// Helper: get unique variables in a condition (simple, limited depth).
void collectVarsInCondition(
    const Value* cond,
    std::set<const Value*>& vars,
    unsigned depth = 0, unsigned maxDepth = 4)
{
    if (!cond || depth > maxDepth) return;
    if (const Instruction* I = dyn_cast<Instruction>(cond)) {
        for (const Use& U : I->operands())
            collectVarsInCondition(U.get(), vars, depth + 1, maxDepth);
    } else if (const Argument* A = dyn_cast<Argument>(cond)) {
        vars.insert(A);
    } else if (const Constant* C = dyn_cast<Constant>(cond)) {
        // skip
    } else {
        vars.insert(cond);
    }
}

// Helper: check if Value is a variable (not a constant).
bool isVariable(const Value* V) {
    return !isa<Constant>(V);
}

// Helper: count variables passed to callees in a function.
unsigned countVarsPassedToCallees(const Function& F) {
    unsigned count = 0;
    for (const Instruction& I : instructions(F)) {
        if (const CallBase* CB = dyn_cast<CallBase>(&I)) {
            for (unsigned ai = 0, ae = CB->arg_size(); ai < ae; ++ai) {
                const Value* arg = CB->getArgOperand(ai);
                if (isVariable(arg)) ++count;
            }
        }
    }
    return count;
}

// Helper: count variables passed to callees in a BB.
unsigned countVarsPassedToCalleesBB(const BasicBlock& BB) {
    unsigned count = 0;
    for (const Instruction& I : BB) {
        if (const CallBase* CB = dyn_cast<CallBase>(&I)) {
            for (unsigned ai = 0, ae = CB->arg_size(); ai < ae; ++ai) {
                const Value* arg = CB->getArgOperand(ai);
                if (isVariable(arg)) ++count;
            }
        }
    }
    return count;
}

// Helper: build set of control blocks in a function.
std::unordered_set<const BasicBlock*> getControlBlocks(const Function& F) {
    std::unordered_set<const BasicBlock*> ctrl;
    for (const BasicBlock& BB : F) {
        if (const Instruction* term = BB.getTerminator()) {
            if (isa<BranchInst>(term) && cast<BranchInst>(term)->isConditional())
                ctrl.insert(&BB);
            else if (isa<SwitchInst>(term) || isa<IndirectBrInst>(term))
                ctrl.insert(&BB);
        }
    }
    return ctrl;
}

// Helper: get all variables in all conditions in function.
void collectVarsInAllConditions(const Function& F, std::set<const Value*>& out) {
    for (const BasicBlock& BB : F) {
        if (const Instruction* term = BB.getTerminator()) {
            if (const BranchInst* BI = dyn_cast<BranchInst>(term)) {
                if (BI->isConditional())
                    collectVarsInCondition(BI->getCondition(), out);
            } else if (const SwitchInst* SI = dyn_cast<SwitchInst>(term)) {
                collectVarsInCondition(SI->getCondition(), out);
            }
        }
    }
}

// Helper: Immediate PostDominator for a BB.
const BasicBlock* getImmediatePostDom(const BasicBlock* BB, const PostDominatorTree* PDT) {
    if (!BB || !PDT) return nullptr;
    if (const DomTreeNodeBase<BasicBlock>* node = PDT->getNode(BB))
        if (node->getIDom()) return node->getIDom()->getBlock();
    return nullptr;
}

// Helper: chain length of control-dependent chain (max nesting level among control blocks).
unsigned getMaxControlDependentChain(const Function& F, const DominatorTree* DT) {
    unsigned maxChain = 0;
    auto ctrlBlocks = getControlBlocks(F);
    for (const BasicBlock* BB : ctrlBlocks) {
        unsigned level = computeNestingLevel(BB, DT, ctrlBlocks);
        if (level > maxChain) maxChain = level;
    }
    return maxChain;
}

// Helper: max data-dependent chain in control blocks (approx).
unsigned getMaxDataDependentChain(const Function& F, const DominatorTree* DT) {
    // For each control block, walk up idom chain, check if condition variable sets intersect with any ancestor.
    auto ctrlBlocks = getControlBlocks(F);
    std::map<const BasicBlock*, std::set<const Value*>> condVars;
    for (const BasicBlock* BB : ctrlBlocks) {
        if (const Instruction* term = BB->getTerminator()) {
            if (const BranchInst* BI = dyn_cast<BranchInst>(term)) {
                if (BI->isConditional()) {
                    std::set<const Value*> vars;
                    collectVarsInCondition(BI->getCondition(), vars);
                    condVars[BB] = vars;
                }
            } else if (const SwitchInst* SI = dyn_cast<SwitchInst>(term)) {
                std::set<const Value*> vars;
                collectVarsInCondition(SI->getCondition(), vars);
                condVars[BB] = vars;
            }
        }
    }
    unsigned maxChain = 0;
    for (const BasicBlock* BB : ctrlBlocks) {
        unsigned chain = 1;
        std::set<const Value*> seenVars = condVars[BB];
        const BasicBlock* curr = BB;
        while (const DomTreeNodeBase<BasicBlock>* node = DT->getNode(curr)) {
            curr = node->getIDom() ? node->getIDom()->getBlock() : nullptr;
            if (curr && ctrlBlocks.count(curr)) {
                if (!condVars[curr].empty()) {
                    bool intersects = false;
                    for (const Value* v : condVars[curr]) {
                        if (seenVars.count(v)) { intersects = true; break; }
                    }
                    if (intersects) {
                        seenVars.insert(condVars[curr].begin(), condVars[curr].end());
                        ++chain;
                    } else break;
                }
            }
        }
        if (chain > maxChain) maxChain = chain;
    }
    return maxChain;
}

} // end anonymous namespace

namespace SVF
{

LeopardCResult getLeopardC(VersionedFlowSensitive* fspta, SVFG* svfg)
{
    LeopardCResult result;
    using namespace llvm;

    // Get the LLVM module.
    const SVFModule* svfModule = fspta->getSVFModule();
    const Module* M = svfModule ? svfModule->getLLVMModule() : nullptr;
    if (!M) return result;

    for (const Function& F : *M) {
        if (F.isDeclaration() || F.empty()) continue;

        // LoopInfo and DominatorTree.
        DominatorTree DT(const_cast<Function&>(F));
        LoopInfo LI(DT);

        // Cyclomatic: edges - nodes + 2
        unsigned nodes = 0, edges = 0;
        for (const BasicBlock& BB : F) {
            ++nodes;
            if (const Instruction* term = BB.getTerminator())
                edges += term->getNumSuccessors();
        }
        unsigned cyclomatic = edges - nodes + 2;

        // Loops.
        std::vector<const Loop*> loops;
        collectLoops(LI, loops);
        unsigned loopCount = loops.size();
        unsigned nestedLoopCount = 0, maxLoopNesting = 0;
        for (const Loop* L : loops) {
            if (!L->getSubLoops().empty()) ++nestedLoopCount;
            unsigned depth = getMaxLoopNesting(L);
            if (depth > maxLoopNesting) maxLoopNesting = depth;
        }

        LeopardFunctionId fid { &F, getFunctionName(&F) };
        LeopardFunctionComplexity fc { fid, cyclomatic, loopCount, nestedLoopCount, maxLoopNesting };
        result.functionsSorted.push_back(fc);

        // BB complexities.
        auto bbIndices = getBasicBlockIndices(F);
        for (const BasicBlock& BB : F) {
            unsigned loopDepth = LI.getLoopDepth(&BB);
            unsigned outDegree = BB.getTerminator() ? BB.getTerminator()->getNumSuccessors() : 0;
            bool isLoopHeader = false;
            if (const Loop* L = LI.getLoopFor(&BB))
                isLoopHeader = (L->getHeader() == &BB);
            LeopardBasicBlockId bid { &BB, fid.name, bbIndices[&BB] };
            LeopardBasicBlockComplexity bc { bid, loopDepth, outDegree, isLoopHeader };
            result.basicBlocksSorted.push_back(bc);
        }
    }

    // Sorting: functionsSorted: cyclomatic, maxLoopNesting, loopCount, nestedLoopCount (all desc)
    std::sort(result.functionsSorted.begin(), result.functionsSorted.end(),
        [](const LeopardFunctionComplexity& a, const LeopardFunctionComplexity& b) {
            if (a.cyclomatic != b.cyclomatic) return a.cyclomatic > b.cyclomatic;
            if (a.maxLoopNesting != b.maxLoopNesting) return a.maxLoopNesting > b.maxLoopNesting;
            if (a.loopCount != b.loopCount) return a.loopCount > b.loopCount;
            return a.nestedLoopCount > b.nestedLoopCount;
        });

    // basicBlocksSorted: loopDepth, then outDegree (desc)
    std::sort(result.basicBlocksSorted.begin(), result.basicBlocksSorted.end(),
        [](const LeopardBasicBlockComplexity& a, const LeopardBasicBlockComplexity& b) {
            if (a.loopDepth != b.loopDepth) return a.loopDepth > b.loopDepth;
            return a.outDegree > b.outDegree;
        });

    return result;
}

LeopardVResult getLeopardV(VersionedFlowSensitive* fspta, SVFG* svfg)
{
    LeopardVResult result;
    using namespace llvm;

    const SVFModule* svfModule = fspta->getSVFModule();
    const Module* M = svfModule ? svfModule->getLLVMModule() : nullptr;
    if (!M) return result;

    for (const Function& F : *M) {
        if (F.isDeclaration() || F.empty()) continue;

        DominatorTree DT(const_cast<Function&>(F));
        PostDominatorTree PDT(const_cast<Function&>(F));
        LoopInfo LI(DT);

        // Dependency
        DependencyMetricsFn depFn;
        depFn.paramCount = F.arg_size();
        depFn.variablesPassedToCallees = countVarsPassedToCallees(F);

        // Pointers
        unsigned ptrArithOps = 0;
        std::unordered_map<const Value*, unsigned> varHistogram;
        analyzePointerOps(F, ptrArithOps, varHistogram);
        unsigned variablesInvolved = varHistogram.size();
        unsigned maxOpsPerVar = 0;
        for (const auto& kv : varHistogram)
            if (kv.second > maxOpsPerVar) maxOpsPerVar = kv.second;
        PointerMetricsFn ptrFn { ptrArithOps, variablesInvolved, maxOpsPerVar };

        // Control
        ControlMetricsFn ctrlFn {};
        auto ctrlBlocks = getControlBlocks(F);

        // Compute nestingLevel for all BBs, and sum for nestedControlCount.
        std::unordered_map<const BasicBlock*, unsigned> nestingLevels;
        unsigned maxNestingLevel = 0, sumNested = 0;
        for (const BasicBlock& BB : F) {
            unsigned level = computeNestingLevel(&BB, &DT, ctrlBlocks);
            nestingLevels[&BB] = level;
            if (level > maxNestingLevel) maxNestingLevel = level;
            sumNested += level;
        }
        ctrlFn.nestedControlCount = sumNested;
        ctrlFn.maxNestingLevel = maxNestingLevel;
        ctrlFn.maxControlDependentChain = getMaxControlDependentChain(F, &DT);
        ctrlFn.maxDataDependentChain = getMaxDataDependentChain(F, &DT);

        // IfWithoutElse
        unsigned ifWithoutElseCount = 0;
        for (const BasicBlock& BB : F) {
            if (const BranchInst* BI = dyn_cast<BranchInst>(BB.getTerminator())) {
                if (BI->isConditional()) {
                    const BasicBlock* ipdom = getImmediatePostDom(&BB, &PDT);
                    bool ok = false;
                    for (unsigned i = 0, e = BI->getNumSuccessors(); i < e; ++i)
                        if (BI->getSuccessor(i) == ipdom) ok = true;
                    if (ok) ++ifWithoutElseCount;
                }
            }
        }
        ctrlFn.ifWithoutElseCount = ifWithoutElseCount;

        // Variables in conditions
        std::set<const Value*> condVars;
        collectVarsInAllConditions(F, condVars);
        ctrlFn.variablesInConditions = condVars.size();

        LeopardFunctionId fid { &F, getFunctionName(&F) };
        LeopardFunctionVuln fVuln { fid, depFn, ptrFn, ctrlFn };
        result.functionsDependencySorted.push_back(fVuln);
        result.functionsPointersSorted.push_back(fVuln);
        result.functionsControlSorted.push_back(fVuln);

        // BB metrics
        auto bbIndices = getBasicBlockIndices(F);

        for (const BasicBlock& BB : F) {
            LeopardBasicBlockId bid { &BB, fid.name, bbIndices[&BB] };

            // Dependency
            DependencyMetricsBB depBB;
            depBB.variablesPassedToCallees = countVarsPassedToCalleesBB(BB);

            // Pointer
            unsigned bbPtrArithOps = 0;
            std::unordered_map<const Value*, unsigned> bbVarHistogram;
            analyzePointerOpsBB(BB, bbPtrArithOps, bbVarHistogram);
            unsigned bbVariablesInvolved = bbVarHistogram.size();
            unsigned bbMaxOpsPerVar = 0;
            for (const auto& kv : bbVarHistogram)
                if (kv.second > bbMaxOpsPerVar) bbMaxOpsPerVar = kv.second;
            PointerMetricsBB ptrBB { bbPtrArithOps, bbVariablesInvolved, bbMaxOpsPerVar };

            // Control
            ControlMetricsBB ctrlBB {};
            ctrlBB.nestingLevel = nestingLevels[&BB];
            ctrlBB.isConditionalBlock = false;
            ctrlBB.variablesInConditions = 0;
            if (const Instruction* term = BB.getTerminator()) {
                if (const BranchInst* BI = dyn_cast<BranchInst>(term)) {
                    if (BI->isConditional()) {
                        ctrlBB.isConditionalBlock = true;
                        std::set<const Value*> vars;
                        collectVarsInCondition(BI->getCondition(), vars);
                        ctrlBB.variablesInConditions = vars.size();
                    }
                } else if (const SwitchInst* SI = dyn_cast<SwitchInst>(term)) {
                    ctrlBB.isConditionalBlock = true;
                    std::set<const Value*> vars;
                    collectVarsInCondition(SI->getCondition(), vars);
                    ctrlBB.variablesInConditions = vars.size();
                }
            }

            LeopardBasicBlockVuln bbVuln { bid, depBB, ptrBB, ctrlBB };
            result.basicBlocksDependencySorted.push_back(bbVuln);
            result.basicBlocksPointersSorted.push_back(bbVuln);
            result.basicBlocksControlSorted.push_back(bbVuln);
        }
    }

    // Sorting for functions
    auto depFnSort = [](const LeopardFunctionVuln& a, const LeopardFunctionVuln& b) {
        if (a.dependency.variablesPassedToCallees != b.dependency.variablesPassedToCallees)
            return a.dependency.variablesPassedToCallees > b.dependency.variablesPassedToCallees;
        return a.dependency.paramCount > b.dependency.paramCount;
    };
    std::sort(result.functionsDependencySorted.begin(), result.functionsDependencySorted.end(), depFnSort);

    auto ptrFnSort = [](const LeopardFunctionVuln& a, const LeopardFunctionVuln& b) {
        if (a.pointers.ptrArithOps != b.pointers.ptrArithOps)
            return a.pointers.ptrArithOps > b.pointers.ptrArithOps;
        if (a.pointers.maxOpsPerVar != b.pointers.maxOpsPerVar)
            return a.pointers.maxOpsPerVar > b.pointers.maxOpsPerVar;
        return a.pointers.variablesInvolved > b.pointers.variablesInvolved;
    };
    std::sort(result.functionsPointersSorted.begin(), result.functionsPointersSorted.end(), ptrFnSort);

    auto ctrlFnSort = [](const LeopardFunctionVuln& a, const LeopardFunctionVuln& b) {
        if (a.control.maxNestingLevel != b.control.maxNestingLevel)
            return a.control.maxNestingLevel > b.control.maxNestingLevel;
        if (a.control.ifWithoutElseCount != b.control.ifWithoutElseCount)
            return a.control.ifWithoutElseCount > b.control.ifWithoutElseCount;
        if (a.control.maxControlDependentChain != b.control.maxControlDependentChain)
            return a.control.maxControlDependentChain > b.control.maxControlDependentChain;
        if (a.control.maxDataDependentChain != b.control.maxDataDependentChain)
            return a.control.maxDataDependentChain > b.control.maxDataDependentChain;
        return a.control.variablesInConditions > b.control.variablesInConditions;
    };
    std::sort(result.functionsControlSorted.begin(), result.functionsControlSorted.end(), ctrlFnSort);

    // Sorting for BBs: dependency
    auto depBBSort = [](const LeopardBasicBlockVuln& a, const LeopardBasicBlockVuln& b) {
        return a.dependency.variablesPassedToCallees > b.dependency.variablesPassedToCallees;
    };
    std::sort(result.basicBlocksDependencySorted.begin(), result.basicBlocksDependencySorted.end(), depBBSort);

    // pointer
    auto ptrBBSort = [](const LeopardBasicBlockVuln& a, const LeopardBasicBlockVuln& b) {
        if (a.pointers.ptrArithOps != b.pointers.ptrArithOps)
            return a.pointers.ptrArithOps > b.pointers.ptrArithOps;
        if (a.pointers.maxOpsPerVar != b.pointers.maxOpsPerVar)
            return a.pointers.maxOpsPerVar > b.pointers.maxOpsPerVar;
        return a.pointers.variablesInvolved > b.pointers.variablesInvolved;
    };
    std::sort(result.basicBlocksPointersSorted.begin(), result.basicBlocksPointersSorted.end(), ptrBBSort);

    // control
    auto ctrlBBSort = [](const LeopardBasicBlockVuln& a, const LeopardBasicBlockVuln& b) {
        if (a.control.nestingLevel != b.control.nestingLevel)
            return a.control.nestingLevel > b.control.nestingLevel;
        if (a.control.isConditionalBlock != b.control.isConditionalBlock)
            return a.control.isConditionalBlock > b.control.isConditionalBlock;
        return a.control.variablesInConditions > b.control.variablesInConditions;
    };
    std::sort(result.basicBlocksControlSorted.begin(), result.basicBlocksControlSorted.end(), ctrlBBSort);

    return result;
}

} // namespace SVF