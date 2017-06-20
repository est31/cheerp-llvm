//===- Interpreter.cpp - Top-Level LLVM Interpreter Implementation --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the top-level functionality for the LLVM interpreter.
// This interpreter is designed to be a very simple, portable, inefficient
// interpreter.
//
//===----------------------------------------------------------------------===//

#include "Interpreter.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include <cstring>
using namespace llvm;

namespace {

static struct RegisterInterp {
  RegisterInterp() { Interpreter::Register(); }
} InterpRegistrator;

}

extern "C" void LLVMLinkInInterpreter() { }

/// Create a new interpreter object.
///
ExecutionEngine *Interpreter::create(std::unique_ptr<Module> M,
                                     bool preExecute,
                                     std::string *ErrStr) {
  // Tell this Module to materialize everything and release the GVMaterializer.
  if (std::error_code EC = M->materializeAllPermanently()) {
    if (ErrStr)
      *ErrStr = EC.message();
    // We got an error, just return 0
    return nullptr;
  }

  return new Interpreter(std::move(M), preExecute);
}

//===----------------------------------------------------------------------===//
// Interpreter ctor - Initialize stuff
//
Interpreter::Interpreter(std::unique_ptr<Module> M, bool preExecute)
  : ExecutionEngine(std::move(M)), TD(Modules.back().get()), ForPreExecute(preExecute), CleanAbort(false) {

  if (ForPreExecute) {
    MemoryAllocator = std::unique_ptr<VirtualAllocatorBase>(new VirtualAllocator());
    FunctionAddresses = std::unique_ptr<FunctionMapBase>(new VirtualFunctionMap());
  }
  memset(&ExitValue.Untyped, 0, sizeof(ExitValue.Untyped));
  setDataLayout(&TD);
  // Initialize the "backend"
  initializeExecutionEngine();
  initializeExternalFunctions();
  // Allow unresolved globals if pre-executing
  emitGlobals(/* AllowUnresolved */ ForPreExecute);

  IL = new IntrinsicLowering(TD);
}

Interpreter::~Interpreter() {
  delete IL;
}

void Interpreter::runAtExitHandlers () {
  while (!AtExitHandlers.empty()) {
    callFunction(AtExitHandlers.back(), std::vector<GenericValue>());
    AtExitHandlers.pop_back();
    run();
  }
}

/// run - Start execution with the specified function and arguments.
///
GenericValue
Interpreter::runFunction(Function *F,
                         const std::vector<GenericValue> &ArgValues) {
  assert (F && "Function *F was null at entry to run()");

  // Try extra hard not to pass extra args to a function that isn't
  // expecting them.  C programmers frequently bend the rules and
  // declare main() with fewer parameters than it actually gets
  // passed, and the interpreter barfs if you pass a function more
  // parameters than it is declared to take. This does not attempt to
  // take into account gratuitous differences in declared types,
  // though.
  std::vector<GenericValue> ActualArgs;
  const unsigned ArgCount = F->getFunctionType()->getNumParams();
  for (unsigned i = 0; i < ArgCount; ++i)
    ActualArgs.push_back(ArgValues[i]);

  // Set up the function call.
  callFunction(F, ActualArgs);

  // Start executing the function.
  run();

  return ExitValue;
}
