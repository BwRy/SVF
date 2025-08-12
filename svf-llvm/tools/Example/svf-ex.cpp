//===- svf-ex.cpp -- A driver example of SVF-------------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===-----------------------------------------------------------------------===//

/*
 // A driver program of SVF including usages of SVF APIs
 //
 // Author: Yulei Sui,
 */

#include "AE/Core/AbstractState.h"
#include "Graphs/SVFG.h"
#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"
#include "SVF-LLVM/LeopardMetrics.h"

using namespace llvm;
using namespace std;
using namespace SVF;

/*!
 * An example to query alias results of two SVF values
 */
SVF::AliasResult aliasQuery(PointerAnalysis* pta, const SVFVar* v1, const SVFVar* v2)
{
    return pta->alias(v1->getId(), v2->getId());
}

/*!
 * An example to print points-to set of an SVF value
 */
std::string printPts(PointerAnalysis* pta, const SVFVar* svfval)
{

    std::string str;
    raw_string_ostream rawstr(str);

    NodeID pNodeId = svfval->getId();
    const PointsTo& pts = pta->getPts(pNodeId);
    for (PointsTo::iterator ii = pts.begin(), ie = pts.end();
            ii != ie; ii++)
    {
        rawstr << " " << *ii << " ";
        PAGNode* targetObj = pta->getPAG()->getGNode(*ii);
        rawstr << "(" << targetObj->toString() << ")\t ";
    }

    return rawstr.str();

}
/*!
 * An example to query/collect all successor nodes from a ICFGNode (iNode) along control-flow graph (ICFG)
 */
void traverseOnICFG(ICFG* icfg, const ICFGNode* iNode)
{
    FIFOWorkList<const ICFGNode*> worklist;
    Set<const ICFGNode*> visited;
    worklist.push(iNode);

    /// Traverse along VFG
    while (!worklist.empty())
    {
        const ICFGNode* vNode = worklist.pop();
        for (ICFGNode::const_iterator it = vNode->OutEdgeBegin(), eit =
                    vNode->OutEdgeEnd(); it != eit; ++it)
        {
            ICFGEdge* edge = *it;
            ICFGNode* succNode = edge->getDstNode();
            if (visited.find(succNode) == visited.end())
            {
                visited.insert(succNode);
                worklist.push(succNode);
            }
        }
    }
}

void dummyVisit(const VFGNode* node)
{

}
/*!
 * An example to query/collect all the uses of a definition of a value along value-flow graph (VFG)
 */
void traverseOnVFG(const SVFG* vfg, const SVFVar* svfval)
{
    if (!vfg->hasDefSVFGNode(svfval))
        return;
    const VFGNode* vNode = vfg->getDefSVFGNode(svfval);
    FIFOWorkList<const VFGNode*> worklist;
    Set<const VFGNode*> visited;
    worklist.push(vNode);

    /// Traverse along VFG
    while (!worklist.empty())
    {
        const VFGNode* vNode = worklist.pop();
        for (VFGNode::const_iterator it = vNode->OutEdgeBegin(), eit =
                    vNode->OutEdgeEnd(); it != eit; ++it)
        {
            VFGEdge* edge = *it;
            VFGNode* succNode = edge->getDstNode();
            if (visited.find(succNode) == visited.end())
            {
                visited.insert(succNode);
                worklist.push(succNode);
            }
        }
    }

    /// Collect all LLVM Values
    for(Set<const VFGNode*>::const_iterator it = visited.begin(), eit = visited.end(); it!=eit; ++it)
    {
        const VFGNode* node = *it;
        dummyVisit(node);
        /// can only query VFGNode involving top-level pointers (starting with % or @ in LLVM IR)
        /// PAGNode* pNode = vfg->getLHSTopLevPtr(node);
        /// Value* val = pNode->getValue();
    }
}

int main(int argc, char ** argv)
{

    std::vector<std::string> moduleNameVec;
    moduleNameVec = OptionBase::parseOptions(
                        argc, argv, "Whole Program Points-to Analysis", "[options] <input-bitcode...>"
                    );

    if (Options::WriteAnder() == "ir_annotator")
    {
        LLVMModuleSet::preProcessBCs(moduleNameVec);
    }

    LLVMModuleSet::buildSVFModule(moduleNameVec);

    /// Build Program Assignment Graph (SVFIR)
    SVFIRBuilder builder;
    SVFIR* pag = builder.build();

    /// Create Andersen's pointer analysis
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);


    /// Call Graph
    CallGraph* callgraph = ander->getCallGraph();

    /// ICFG
    ICFG* icfg = pag->getICFG();

    /// Value-Flow Graph (VFG)
    VFG* vfg = new VFG(callgraph);

    /// Sparse value-flow graph (SVFG)
    SVFGBuilder svfBuilder;
    SVFG* svfg = svfBuilder.buildFullSVFG(ander);

    /// Collect uses of an LLVM Value
    if (Options::PTSPrint())
    {
        for (const auto& it : *svfg)
        {
            const SVFGNode* node = it.second;
            if (node->getValue())
            {
                traverseOnVFG(svfg, node->getValue());
                /// Print points-to information
                printPts(ander, node->getValue());
                for (const SVFGEdge* edge : node->getOutEdges())
                {
                    const SVFGNode* node2 = edge->getDstNode();
                    if (node2->getValue())
                        aliasQuery(ander, node->getValue(), node2->getValue());
                }
            }
        }
    }

    /// Collect all successor nodes on ICFG
    if (Options::PTSPrint())
    {
        for (const auto& it : *icfg)
        {
            const ICFGNode* node = it.second;
            traverseOnICFG(icfg, node);
        }
    }

    // Leopard metrics demo flag
    static llvm::cl::opt<bool> LeopardFlag("leopard", llvm::cl::desc("Run Leopard metrics analysis"), llvm::cl::init(false));
    if (LeopardFlag) {
        outs() << "Running Leopard Complexity Analysis...\n";
        auto cres = SVF::getLeopardC(nullptr, svfg);
        outs() << "Top 10 Functions by Cyclomatic Complexity:\n";
        for (size_t i = 0; i < std::min<size_t>(10, cres.functionsSorted.size()); ++i) {
            const auto& f = cres.functionsSorted[i];
            outs() << "  " << f.id.name << " : cyclomatic=" << f.cyclomatic
                   << ", maxLoopNesting=" << f.maxLoopNesting
                   << ", loopCount=" << f.loopCount
                   << ", nestedLoopCount=" << f.nestedLoopCount << "\n";
        }
        outs() << "Top 10 BasicBlocks by Loop Depth:\n";
        for (size_t i = 0; i < std::min<size_t>(10, cres.basicBlocksSorted.size()); ++i) {
            const auto& bb = cres.basicBlocksSorted[i];
            outs() << "  " << bb.id.functionName << " BB#" << bb.id.indexInFunction
                   << " : loopDepth=" << bb.loopDepth << ", outDegree=" << bb.outDegree
                   << ", isLoopHeader=" << bb.isLoopHeader << "\n";
        }
        outs() << "Running Leopard Vulnerability Analysis...\n";
        auto vres = SVF::getLeopardV(nullptr, svfg);
        outs() << "Top 10 Functions by Variables Passed to Callees:\n";
        for (size_t i = 0; i < std::min<size_t>(10, vres.functionsDependencySorted.size()); ++i) {
            const auto& f = vres.functionsDependencySorted[i];
            outs() << "  " << f.id.name << " : variablesPassedToCallees=" << f.dependency.variablesPassedToCallees
                   << ", paramCount=" << f.dependency.paramCount << "\n";
        }
        outs() << "Top 10 Functions by Pointer Arithmetic Ops:\n";
        for (size_t i = 0; i < std::min<size_t>(10, vres.functionsPointersSorted.size()); ++i) {
            const auto& f = vres.functionsPointersSorted[i];
            outs() << "  " << f.id.name << " : ptrArithOps=" << f.pointers.ptrArithOps
                   << ", maxOpsPerVar=" << f.pointers.maxOpsPerVar
                   << ", variablesInvolved=" << f.pointers.variablesInvolved << "\n";
        }
        outs() << "Top 10 Functions by Control Complexity:\n";
        for (size_t i = 0; i < std::min<size_t>(10, vres.functionsControlSorted.size()); ++i) {
            const auto& f = vres.functionsControlSorted[i];
            outs() << "  " << f.id.name
                   << " : maxNestingLevel=" << f.control.maxNestingLevel
                   << ", ifWithoutElseCount=" << f.control.ifWithoutElseCount
                   << ", maxControlDependentChain=" << f.control.maxControlDependentChain
                   << ", maxDataDependentChain=" << f.control.maxDataDependentChain
                   << ", variablesInConditions=" << f.control.variablesInConditions << "\n";
        }
    }

    // clean up memory
    delete vfg;
    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();

    LLVMModuleSet::getLLVMModuleSet()->dumpModulesToFile(".svf.bc");
    SVF::LLVMModuleSet::releaseLLVMModuleSet();
    llvm::llvm_shutdown();
    return 0;
}

