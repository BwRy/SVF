#ifndef SVFLLVM_LEOPARDMETRICS_H
#define SVFLLVM_LEOPARDMETRICS_H

#include "SVF-LLVM/VersionedFlowSensitive.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/SVFModule.h"
#include "SVF-LLVM/SVFUtil.h"
#include "SVF/Graphs/SVFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include <vector>
#include <string>
#include <unordered_set>
#include <map>
#include <set>
#include <cstdint>

namespace SVF
{

/*
 * Identifier for a function.
 * Holds pointer to Function and a human-readable name.
 */
struct LeopardFunctionId {
    const llvm::Function* F;    ///< Pointer to Function
    std::string name;           ///< Demangled function name or identifier
};

/*
 * Identifier for a basic block.
 * Holds pointer to BB, function name, and its index in the function.
 */
struct LeopardBasicBlockId {
    const llvm::BasicBlock* BB; ///< Pointer to BasicBlock
    std::string functionName;   ///< Name of parent function
    unsigned indexInFunction;   ///< Index of BB in function (order of appearance)
};

/*
 * Complexity metrics for a function.
 */
struct LeopardFunctionComplexity {
    LeopardFunctionId id;
    unsigned cyclomatic;        ///< Cyclomatic complexity (E - N + 2P)
    unsigned loopCount;         ///< Number of loops in function
    unsigned nestedLoopCount;   ///< Number of loops that contain subloops
    unsigned maxLoopNesting;    ///< Maximum nesting depth of loops
};

/*
 * Complexity metrics for a basic block.
 */
struct LeopardBasicBlockComplexity {
    LeopardBasicBlockId id;
    unsigned loopDepth;         ///< Loop nesting depth for this BB
    unsigned outDegree;         ///< Number of CFG successors (terminator successor count)
    bool isLoopHeader;          ///< Whether BB is a loop header
};

/*
 * Result struct for getLeopardC: holds sorted vectors of function and BB complexity.
 */
struct LeopardCResult {
    std::vector<LeopardFunctionComplexity> functionsSorted;      ///< Desc by cyclomatic, maxLoopNesting, loopCount, nestedLoopCount
    std::vector<LeopardBasicBlockComplexity> basicBlocksSorted;  ///< Desc by loopDepth, outDegree
};

/*
 * Dependency metrics for a function.
 */
struct DependencyMetricsFn {
    unsigned paramCount;               ///< Number of function parameters
    unsigned variablesPassedToCallees; ///< Count of variables (non-constants) passed to callees
};

/*
 * Dependency metrics for a basic block.
 */
struct DependencyMetricsBB {
    unsigned variablesPassedToCallees; ///< Variables passed to callees in this BB
};

/*
 * Pointer metrics for a function.
 */
struct PointerMetricsFn {
    unsigned ptrArithOps;      ///< Pointer arithmetic operations (GEP, PtrToInt, etc.)
    unsigned variablesInvolved;///< Unique variables involved in pointer arithmetic
    unsigned maxOpsPerVar;     ///< Maximum number of pointer ops for a single variable
};

/*
 * Pointer metrics for a basic block.
 */
struct PointerMetricsBB {
    unsigned ptrArithOps;
    unsigned variablesInvolved;
    unsigned maxOpsPerVar;
};

/*
 * Control structure metrics for a function.
 */
struct ControlMetricsFn {
    unsigned nestedControlCount;       ///< Total count of nested control structures
    unsigned maxNestingLevel;          ///< Maximum nesting depth of control structures
    unsigned maxControlDependentChain; ///< Max chain of control-dependent conditionals
    unsigned maxDataDependentChain;    ///< Max chain with intersecting condition variables
    unsigned ifWithoutElseCount;       ///< Branch with immediate post-dominator = one succ
    unsigned variablesInConditions;    ///< Unique variables used in conditions (fn-level)
};

/*
 * Control structure metrics for a basic block.
 */
struct ControlMetricsBB {
    unsigned nestingLevel;             ///< Nesting level of conditionals dominating this BB
    bool isConditionalBlock;           ///< Whether BB ends with a conditional terminator
    unsigned variablesInConditions;    ///< Vars used in this BB’s condition if any
};

/*
 * Vulnerability metrics for a function.
 */
struct LeopardFunctionVuln {
    LeopardFunctionId id;
    DependencyMetricsFn dependency;
    PointerMetricsFn pointers;
    ControlMetricsFn control;
};

/*
 * Vulnerability metrics for a basic block.
 */
struct LeopardBasicBlockVuln {
    LeopardBasicBlockId id;
    DependencyMetricsBB dependency;
    PointerMetricsBB pointers;
    ControlMetricsBB control;
};

/*
 * Result struct for getLeopardV: holds sorted vectors of vuln metrics.
 */
struct LeopardVResult {
    std::vector<LeopardFunctionVuln> functionsDependencySorted; ///< Desc by dependency.variablesPassedToCallees, paramCount
    std::vector<LeopardFunctionVuln> functionsPointersSorted;   ///< Desc by pointers.ptrArithOps, maxOpsPerVar, variablesInvolved
    std::vector<LeopardFunctionVuln> functionsControlSorted;    ///< Desc by control.maxNestingLevel, ifWithoutElseCount, etc.
    std::vector<LeopardBasicBlockVuln> basicBlocksDependencySorted;
    std::vector<LeopardBasicBlockVuln> basicBlocksPointersSorted;
    std::vector<LeopardBasicBlockVuln> basicBlocksControlSorted;
};

// The main entrypoints for analysis. These must be implemented in LeopardMetrics.cpp.

/**
 * Compute function and basic block complexity metrics.
 * Results are sorted as specified in the task.
 */
LeopardCResult getLeopardC(VersionedFlowSensitive* fspta, SVFG* svfg);

/**
 * Compute function and basic block vulnerability metrics.
 * Results are sorted as specified in the task.
 */
LeopardVResult getLeopardV(VersionedFlowSensitive* fspta, SVFG* svfg);

} // namespace SVF

#endif // SVFLLVM_LEOPARDMETRICS_H