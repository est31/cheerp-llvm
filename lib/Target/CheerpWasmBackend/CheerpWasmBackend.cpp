//===-- CheerpWasmBackend.cpp - Backend wrapper for CheerpWriter---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2017 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include "CheerpWasmTargetMachine.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Cheerp/WasmWriter.h"
#include "llvm/Cheerp/Writer.h"
#include "llvm/Cheerp/PointerPasses.h"
#include "llvm/Cheerp/CFGPasses.h"
#include "llvm/Cheerp/ResolveAliases.h"
#include "llvm/Cheerp/AllocaMerging.h"
#include "llvm/Cheerp/Registerize.h"
#include "llvm/Cheerp/ResolveAliases.h"
#include "llvm/Cheerp/SourceMaps.h"
#include "llvm/Cheerp/LinearMemoryHelper.h"
#include "llvm/Cheerp/CommandLine.h"
#include "llvm/Cheerp/Utility.h"

using namespace llvm;

extern "C" void LLVMInitializeCheerpWasmBackendTarget() {
  // Register the target.
  RegisterTargetMachine<CheerpWastTargetMachine> X(TheCheerpWastBackendTarget);
  RegisterTargetMachine<CheerpWasmTargetMachine> Y(TheCheerpWasmBackendTarget);
}

namespace {
  class CheerpWasmWritePass : public ModulePass {
  private:
    formatted_raw_ostream &Out;
    cheerp::CheerpMode cheerpMode;
    static char ID;
    void getAnalysisUsage(AnalysisUsage& AU) const;
  public:
    explicit CheerpWasmWritePass(formatted_raw_ostream &o, cheerp::CheerpMode cheerpMode) :
      ModulePass(ID), Out(o), cheerpMode(cheerpMode) { }
    bool runOnModule(Module &M);
    const char *getPassName() const {
	return "CheerpWasmWritePass";
    }
  };
} // end anonymous namespace.

bool CheerpWasmWritePass::runOnModule(Module& M)
{
  cheerp::PointerAnalyzer &PA = getAnalysis<cheerp::PointerAnalyzer>();
  cheerp::GlobalDepsAnalyzer &GDA = getAnalysis<cheerp::GlobalDepsAnalyzer>();
  cheerp::Registerize &registerize = getAnalysis<cheerp::Registerize>();
  cheerp::LinearMemoryHelper linearHelper(M, cheerp::LinearMemoryHelper::FunctionAddressMode::Wasm, GDA);

  PA.fullResolve();
  PA.computeConstantOffsets(M);
  registerize.assignRegisters(M, PA);

  // Build the ordered list of reserved names
  std::vector<std::string> reservedNames(ReservedNames.begin(), ReservedNames.end());
  std::sort(reservedNames.begin(), reservedNames.end());

  if (WasmLoader.empty())
  {
    cheerp::NameGenerator namegen(M, GDA, registerize, PA, reservedNames, PrettyCode);
    cheerp::CheerpWasmWriter writer(M, Out, PA, registerize, GDA, linearHelper, namegen,
                                    M.getContext(), CheerpHeapSize, !WasmLoader.empty(),
                                    PrettyCode, cheerpMode);
    writer.makeWasm();
  }
  else
  {
    cheerp::SourceMapGenerator* sourceMapGenerator = NULL;
    GDA.forceTypedArrays = ForceTypedArrays;
    if (!SourceMap.empty())
    {
      std::error_code ErrorCode;
      sourceMapGenerator = new cheerp::SourceMapGenerator(SourceMap, SourceMapPrefix, M.getContext(), ErrorCode);
      if (ErrorCode)
      {
         // An error occurred opening the source map file, bail out
         delete sourceMapGenerator;
         llvm::report_fatal_error(ErrorCode.message(), false);
         return false;
      }
    }

    std::error_code ErrorCode;
    llvm::tool_output_file jsFile(WasmLoader.c_str(), ErrorCode, sys::fs::F_None);
    llvm::formatted_raw_ostream jsOut(jsFile.os());

    cheerp::NameGenerator namegen(M, GDA, registerize, PA, reservedNames, PrettyCode);
    cheerp::CheerpWasmWriter wasmWriter(M, Out, PA, registerize, GDA, linearHelper, namegen,
                                    M.getContext(), CheerpHeapSize, !WasmLoader.empty(),
                                    PrettyCode, cheerpMode);
    wasmWriter.makeWasm();

    cheerp::CheerpWriter writer(M, jsOut, PA, registerize, GDA, linearHelper, namegen, nullptr, std::string(),
            sourceMapGenerator, reservedNames, PrettyCode, MakeModule, NoRegisterize, !NoNativeJavaScriptMath,
            !NoJavaScriptMathImul, !NoJavaScriptMathFround, !NoCredits, MeasureTimeToMain, CheerpHeapSize,
            BoundsCheck, DefinedCheck, SymbolicGlobalsAsmJS, WasmFile, ForceTypedArrays);
    writer.makeJS();
    if (ErrorCode)
    {
       // An error occurred opening the wasm loader file, bail out
       llvm::report_fatal_error(ErrorCode.message(), false);
       delete sourceMapGenerator;
       return false;
    }
    jsFile.keep();
    delete sourceMapGenerator;
  }
  return false;
}

void CheerpWasmWritePass::getAnalysisUsage(AnalysisUsage& AU) const
{
  AU.addRequired<cheerp::GlobalDepsAnalyzer>();
  AU.addRequired<cheerp::PointerAnalyzer>();
  AU.addRequired<cheerp::Registerize>();
}

char CheerpWasmWritePass::ID = 0;

namespace {
class CallGlobalConstructorsOnStartPass : public ModulePass {
	private:
		static char ID;
	public:
		CallGlobalConstructorsOnStartPass() : ModulePass(ID) { }
		bool runOnModule(Module &M);
		const char *getPassName() const {
			return "CallGlobalConstructorsOnStartPass";
		}
};
} // end anonymous namespace.

bool CallGlobalConstructorsOnStartPass::runOnModule(Module& M)
{
	// Determine if a function should be constructed that calls the global
	// constructors on start. The function will not be constructed when the
	// wast loader is in use, or when there are no global constructors.
	if (!WasmLoader.empty())
		return false;

	auto constructors = cheerp::ModuleGlobalConstructors(M);
	if (constructors->op_begin() == constructors->op_end())
		return false;

	// Create the function with the call instructions.
	IRBuilder<> builder(M.getContext());
	auto fTy = FunctionType::get(builder.getVoidTy(), false);
	auto stub = Function::Create(fTy, Function::InternalLinkage, "_start", &M);
	stub->setSection("asmjs");

	auto block = BasicBlock::Create(M.getContext(), "entry", stub);
	builder.SetInsertPoint(block);

	for (auto it = constructors->op_begin(); it != constructors->op_end(); ++it)
	{
		assert(isa<ConstantStruct>(it));
		ConstantStruct* cs = cast<ConstantStruct>(it);
		assert(isa<Function>(cs->getAggregateElement(1)));
		Function* F = cast<Function>(cs->getAggregateElement(1));

		if (F->getSection() != StringRef("asmjs"))
			continue;

		builder.CreateCall(F);
	}

	builder.CreateRet(nullptr);

	// Mark the function as jsexport'ed.
	NamedMDNode* node = M.getNamedMetadata("jsexported_methods");
	assert(node && "create jsexported_methods metadata node");
	node->addOperand(MDNode::get(M.getContext(), {
		ConstantAsMetadata::get(stub),
	}));

	return false;
}

char CallGlobalConstructorsOnStartPass::ID = 0;

//===----------------------------------------------------------------------===//
//                       External Interface declaration
//===----------------------------------------------------------------------===//

bool CheerpBaseTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                           formatted_raw_ostream &o,
                                           CodeGenFileType FileType,
                                           bool DisableVerify,
                                           AnalysisID StartAfter,
                                           AnalysisID StopAfter) {
  PM.add(new CallGlobalConstructorsOnStartPass());
  PM.add(createResolveAliasesPass());
  PM.add(createFreeAndDeleteRemovalPass());
  PM.add(cheerp::createGlobalDepsAnalyzerPass());
  PM.add(createPointerArithmeticToArrayIndexingPass());
  PM.add(createPointerToImmutablePHIRemovalPass());
  PM.add(cheerp::createRegisterizePass(true, false));
  PM.add(cheerp::createPointerAnalyzerPass());
  PM.add(cheerp::createAllocaMergingPass());
  PM.add(createIndirectCallOptimizerPass());
  PM.add(createAllocaArraysPass());
  PM.add(cheerp::createAllocaArraysMergingPass());
  PM.add(createDelayAllocasPass());
  PM.add(createRemoveFwdBlocksPass());
  PM.add(createCheerpWritePass(o));
  return false;
}

ModulePass* CheerpWastTargetMachine::createCheerpWritePass(formatted_raw_ostream& o)
{
	return new CheerpWasmWritePass(o, cheerp::CHEERP_MODE_WAST);
}

ModulePass* CheerpWasmTargetMachine::createCheerpWritePass(formatted_raw_ostream& o)
{
	return new CheerpWasmWritePass(o, cheerp::CHEERP_MODE_WASM);
}
