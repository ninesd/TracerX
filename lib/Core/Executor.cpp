//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Executor.h"
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "Searcher.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "ExecutorTimerInfo.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/CommandLine.h"
#include "klee/Common.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprSMTLIBPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/GetElementPtrTypeIterator.h"
#include "klee/util/TxPrintUtil.h"
#include "klee/Config/Version.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/System/MemoryUsage.h"
#include "klee/SolverStats.h"
#include "TxShadowArray.h"
#include "TxTree.h"
#include "TxSpeculation.h"

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
#include "llvm/IR/Function.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/TypeBuilder.h"
#else
#include "llvm/Attributes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"

#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
#include "llvm/Target/TargetData.h"
#else
#include "llvm/DataLayout.h"
#include "llvm/TypeBuilder.h"
#endif
#endif
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
#include "llvm/Support/CallSite.h"
#else
#include "llvm/IR/CallSite.h"
#endif

#ifdef HAVE_ZLIB_H
#include "klee/Internal/Support/CompressionStream.h"
#endif

#include <cassert>
#include <algorithm>
#include <iomanip>
#include <iosfwd>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <sys/mman.h>

#include <errno.h>
#include <cxxabi.h>

using namespace llvm;
using namespace klee;

namespace {
cl::opt<bool> DumpStatesOnHalt(
    "dump-states-on-halt", cl::init(true),
    cl::desc("Dump test cases for all active states on exit (default=on)"));

cl::opt<bool> RandomizeFork(
    "randomize-fork", cl::init(false),
    cl::desc(
        "Randomly swap the true and false states on a fork (default=off)"));

cl::opt<bool> AllowExternalSymCalls(
    "allow-external-sym-calls", cl::init(false),
    cl::desc("Allow calls with symbolic arguments to external functions.  This "
             "concretizes the symbolic arguments.  (default=off)"));

/// The different query logging solvers that can switched on/off
enum PrintDebugInstructionsType {
  STDERR_ALL, ///
  STDERR_SRC,
  STDERR_COMPACT,
  FILE_ALL,    ///
  FILE_SRC,    ///
  FILE_COMPACT ///
};

llvm::cl::list<PrintDebugInstructionsType> DebugPrintInstructions(
    "debug-print-instructions",
    llvm::cl::desc("Log instructions during execution."),
    llvm::cl::values(
        clEnumValN(STDERR_ALL, "all:stderr", "Log all instructions to stderr "
                                             "in format [src, inst_id, "
                                             "llvm_inst]"),
        clEnumValN(STDERR_SRC, "src:stderr",
                   "Log all instructions to stderr in format [src, inst_id]"),
        clEnumValN(STDERR_COMPACT, "compact:stderr",
                   "Log all instructions to stderr in format [inst_id]"),
        clEnumValN(FILE_ALL, "all:file", "Log all instructions to file "
                                         "instructions.txt in format [src, "
                                         "inst_id, llvm_inst]"),
        clEnumValN(FILE_SRC, "src:file", "Log all instructions to file "
                                         "instructions.txt in format [src, "
                                         "inst_id]"),
        clEnumValN(FILE_COMPACT, "compact:file",
                   "Log all instructions to file instructions.txt in format "
                   "[inst_id]"),
        clEnumValEnd),
    llvm::cl::CommaSeparated);
#ifdef HAVE_ZLIB_H
cl::opt<bool> DebugCompressInstructions(
    "debug-compress-instructions", cl::init(false),
    cl::desc("Compress the logged instructions in gzip format."));
#endif

cl::opt<bool> DebugCheckForImpliedValues("debug-check-for-implied-values");

cl::opt<bool>
SimplifySymIndices("simplify-sym-indices", cl::init(false),
                   cl::desc("Simplify symbolic accesses using equalities "
                            "from other constraints (default=off)"));

cl::opt<bool>
EqualitySubstitution("equality-substitution", cl::init(true),
                     cl::desc("Simplify equality expressions before querying "
                              "the solver (default=on)."));

cl::opt<unsigned> MaxSymArraySize("max-sym-array-size", cl::init(0));

cl::opt<bool> SuppressExternalWarnings(
    "suppress-external-warnings", cl::init(false),
    cl::desc("Supress warnings about calling external functions."));

cl::opt<bool> AllExternalWarnings(
    "all-external-warnings", cl::init(false),
    cl::desc("Issue an warning everytime an external call is made,"
             "as opposed to once per function (default=off)"));

cl::opt<bool> OnlyOutputStatesCoveringNew(
    "only-output-states-covering-new", cl::init(false),
    cl::desc("Only output test cases covering new code (default=off)."));

cl::opt<bool>
EmitAllErrors("emit-all-errors", cl::init(false),
              cl::desc("Generate tests cases for all errors "
                       "(default=off, i.e. one per (error,instruction) pair)"));

cl::opt<bool>
NoExternals("no-externals",
            cl::desc("Do not allow external function calls (default=off)"));

cl::opt<bool> AlwaysOutputSeeds("always-output-seeds", cl::init(true));

cl::opt<bool> OnlyReplaySeeds(
    "only-replay-seeds", cl::init(false),
    cl::desc("Discard states that do not have a seed (default=off)."));

cl::opt<bool>
OnlySeed("only-seed", cl::init(false),
         cl::desc("Stop execution after seeding is done without doing "
                  "regular search (default=off)."));

cl::opt<bool>
AllowSeedExtension("allow-seed-extension", cl::init(false),
                   cl::desc("Allow extra (unbound) values to become symbolic "
                            "during seeding (default=false)."));

cl::opt<bool> ZeroSeedExtension("zero-seed-extension", cl::init(false),
                                cl::desc("(default=off)"));

cl::opt<bool> AllowSeedTruncation(
    "allow-seed-truncation", cl::init(false),
    cl::desc("Allow smaller buffers than in seeds (default=off)."));

cl::opt<bool> NamedSeedMatching(
    "named-seed-matching", cl::init(false),
    cl::desc("Use names to match symbolic objects to inputs (default=off)."));

cl::opt<double> MaxStaticForkPct("max-static-fork-pct", cl::init(1.),
                                 cl::desc("(default=1.0)"));

cl::opt<double> MaxStaticSolvePct("max-static-solve-pct", cl::init(1.),
                                  cl::desc("(default=1.0)"));

cl::opt<double> MaxStaticCPForkPct("max-static-cpfork-pct", cl::init(1.),
                                   cl::desc("(default=1.0)"));

cl::opt<double> MaxStaticCPSolvePct("max-static-cpsolve-pct", cl::init(1.),
                                    cl::desc("(default=1.0)"));

cl::opt<double> MaxInstructionTime(
    "max-instruction-time",
    cl::desc("Only allow a single instruction to take this much time "
             "(default=0s (off)). Enables --use-forked-solver"),
    cl::init(0));

cl::opt<double> SeedTime("seed-time",
                         cl::desc("Amount of time to dedicate to seeds, before "
                                  "normal search (default=0 (off))"),
                         cl::init(0));

cl::list<Executor::TerminateReason> ExitOnErrorType(
    "exit-on-error-type", cl::desc("Stop execution after reaching a "
                                   "specified condition.  (default=off)"),
    cl::values(
        clEnumValN(Executor::Abort, "Abort", "The program crashed"),
        clEnumValN(Executor::Assert, "Assert", "An assertion was hit"),
        clEnumValN(Executor::Exec, "Exec",
                   "Trying to execute an unexpected instruction"),
        clEnumValN(Executor::External, "External",
                   "External objects referenced"),
        clEnumValN(Executor::Free, "Free", "Freeing invalid memory"),
        clEnumValN(Executor::Model, "Model", "Memory model limit hit"),
        clEnumValN(Executor::Overflow, "Overflow", "An overflow occurred"),
        clEnumValN(Executor::Ptr, "Ptr", "Pointer error"),
        clEnumValN(Executor::ReadOnly, "ReadOnly", "Write to read-only memory"),
        clEnumValN(Executor::ReportError, "ReportError",
                   "klee_report_error called"),
        clEnumValN(Executor::User, "User", "Wrong klee_* functions invocation"),
        clEnumValN(Executor::Unhandled, "Unhandled",
                   "Unhandled instruction hit"),
        clEnumValEnd),
    cl::ZeroOrMore);

cl::opt<unsigned int>
StopAfterNInstructions("stop-after-n-instructions",
                       cl::desc("Stop execution after specified number of "
                                "instructions (default=0 (off))"),
                       cl::init(0));

cl::opt<unsigned>
MaxForks("max-forks", cl::desc("Only fork this many times (default=-1 (off))"),
         cl::init(~0u));

cl::opt<unsigned>
MaxDepth("max-depth",
         cl::desc("Only allow this many symbolic branches (default=0 (off))"),
         cl::init(0));

cl::opt<unsigned> MaxMemory("max-memory",
                            cl::desc("Refuse to fork when above this amount of "
                                     "memory (in MB, default=2000)"),
                            cl::init(2000));

cl::opt<bool> MaxMemoryInhibit(
    "max-memory-inhibit",
    cl::desc(
        "Inhibit forking at memory cap (vs. random terminate) (default=on)"),
    cl::init(true));
} // namespace

namespace klee {
RNG theRNG;
}

const char *Executor::TerminateReasonNames[] =
    {[Abort] = "abort",       [Assert] = "assert",
     [Exec] = "exec",         [External] = "external",
     [Free] = "free",         [Model] = "model",
     [Overflow] = "overflow", [Ptr] = "ptr",
     [ReadOnly] = "readonly", [ReportError] = "reporterror",
     [User] = "user",         [Unhandled] = "xxx", };

Executor::Executor(const InterpreterOptions &opts, InterpreterHandler *ih)
    : Interpreter(opts), kmodule(0), interpreterHandler(ih), searcher(0),
      externalDispatcher(new ExternalDispatcher()), statsTracker(0),
      pathWriter(0), symPathWriter(0), specialFunctionHandler(0),
      processTree(0), txTree(0), replayKTest(0), replayPath(0), usingSeeds(0),
      atMemoryLimit(false), inhibitForking(false), haltExecution(false),
      ivcEnabled(false),
      coreSolverTimeout(MaxCoreSolverTime != 0 && MaxInstructionTime != 0
                            ? std::min(MaxCoreSolverTime, MaxInstructionTime)
                            : std::max(MaxCoreSolverTime, MaxInstructionTime)),
      debugInstFile(0), debugLogBuffer(debugBufferString) {

  // Basic Block Coverage Counters
  if (BBCoverage >= 1) {
    allBlockCount = 0;
    allBlockCollected = false;
    blockCoverage = 0;
    allICMPCount = 0;
    coveredICMPCount = 0;
  }

  if (coreSolverTimeout)
    UseForkedCoreSolver = true;
  Solver *coreSolver = klee::createCoreSolver(CoreSolverToUse);
  if (!coreSolver) {
    klee_error("Failed to create core solver\n");
  }
  Solver *solver = constructSolverChain(
      coreSolver,
      interpreterHandler->getOutputFilename(ALL_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_SMT2_FILE_NAME),
      interpreterHandler->getOutputFilename(ALL_QUERIES_PC_FILE_NAME),
      interpreterHandler->getOutputFilename(SOLVER_QUERIES_PC_FILE_NAME));

  this->solver = new TimingSolver(solver, EqualitySubstitution);
  memory = new MemoryManager(&arrayCache);

  if (optionIsSet(DebugPrintInstructions, FILE_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_COMPACT) ||
      optionIsSet(DebugPrintInstructions, FILE_SRC)) {
    std::string debug_file_name =
        interpreterHandler->getOutputFilename("instructions.txt");
    std::string ErrorInfo;
#ifdef HAVE_ZLIB_H
    if (!DebugCompressInstructions) {
#endif

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 5)
      debugInstFile = new llvm::raw_fd_ostream(
          debug_file_name.c_str(), ErrorInfo, llvm::sys::fs::OpenFlags::F_Text),
#else
    debugInstFile =
        new llvm::raw_fd_ostream(debug_file_name.c_str(), ErrorInfo);
#endif
#ifdef HAVE_ZLIB_H
    } else {
      debugInstFile = new compressed_fd_ostream(
          (debug_file_name + ".gz").c_str(), ErrorInfo);
    }
#endif
    if (ErrorInfo != "") {
      klee_error("Could not open file %s : %s", debug_file_name.c_str(),
                 ErrorInfo.c_str());
    }
  }
}

const Module *Executor::setModule(llvm::Module *module,
                                  const ModuleOptions &opts) {
  assert(!kmodule && module && "can only register one module"); // XXX gross

  kmodule = new KModule(module);

// Initialize the context.
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
  TargetData *TD = kmodule->targetData;
#else
  DataLayout *TD = kmodule->targetData;
#endif
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width)TD->getPointerSizeInBits());

  specialFunctionHandler = new SpecialFunctionHandler(*this);

  specialFunctionHandler->prepare();
  kmodule->prepare(opts, interpreterHandler);
  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics() || userSearcherRequiresMD2U()) {
    statsTracker = new StatsTracker(
        *this, interpreterHandler->getOutputFilename("assembly.ll"),
        userSearcherRequiresMD2U());
  }

  return module;
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  if (processTree)
    delete processTree;
  if (specialFunctionHandler)
    delete specialFunctionHandler;
  if (statsTracker)
    delete statsTracker;
  delete solver;
  delete kmodule;
  while (!timers.empty()) {
    delete timers.back();
    timers.pop_back();
  }
  if (debugInstFile) {
    delete debugInstFile;
  }
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      const Constant *c, unsigned offset) {
#if LLVM_VERSION_CODE <= LLVM_VERSION(3, 1)
  TargetData *targetData = kmodule->targetData;
#else
  DataLayout *targetData = kmodule->targetData;
#endif
  if (const ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
        targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i = 0, e = cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i),
                             offset + i * elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i = 0; i < size; i++)
      os->write8(offset + i, (uint8_t)0);
  } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
        targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i = 0, e = ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i),
                             offset + i * elementSize);
  } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
        targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i = 0, e = cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i),
                             offset + sl->getElementOffset(i));
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
  } else if (const ConstantDataSequential *cds =
                 dyn_cast<ConstantDataSequential>(c)) {
    unsigned elementSize = targetData->getTypeStoreSize(cds->getElementType());
    for (unsigned i = 0, e = cds->getNumElements(); i != e; ++i)
      initializeGlobalObject(state, os, cds->getElementAsConstant(i),
                             offset + i * elementSize);
#endif
  } else if (!isa<UndefValue>(c)) {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject *Executor::addExternalObject(ExecutionState &state, void *addr,
                                          unsigned size, bool isReadOnly) {
  MemoryObject *mo =
      memory->allocateFixed((uint64_t)(unsigned long)addr, size, 0);
  ObjectState *os = bindObjectInState(state, mo, false);
  for (unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t *)addr)[i]);
  if (isReadOnly)
    os->setReadOnly(true);
  return mo;
}

extern void *__dso_handle __attribute__((__weak__));

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module;

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 3)
  assert(m->lib_begin() == m->lib_end() &&
         "XXX do not support dependent libraries");
#endif
  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() &&
        !externalDispatcher->resolveSymbol(f->getName())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer((unsigned long)(void *)f);
      legalFunctions.insert((uint64_t)(unsigned long)(void *)f);
    }

    globalAddresses.insert(std::make_pair(f, addr));
  }

// Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  int *errno_addr = __errno_location();
  addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false);

  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, const_cast<uint16_t *>(*addr - 128),
                    384 * sizeof **addr, true);
  addExternalObject(state, addr, sizeof(*addr), true);

  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, const_cast<int32_t *>(*lower_addr - 128),
                    384 * sizeof **lower_addr, true);
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true);

  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, const_cast<int32_t *>(*upper_addr - 128),
                    384 * sizeof **upper_addr, true);
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true);
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(), e = m->global_end();
       i != e; ++i) {
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = 0;
      if (ty->isSized()) {
        size = kmodule->targetData->getTypeStoreSize(ty);
      } else {
        klee_warning("Type for %.*s is not sized", (int)i->getName().size(),
                     i->getName().data());
      }

// XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        klee_warning("Unable to find size for global variable: %.*s (use will "
                     "result in out of bounds access)",
                     (int)i->getName().size(), i->getName().data());
      }

      MemoryObject *mo = memory->allocate(size, false, true, i);
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getName());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.",
                     i->getName().data());

        for (unsigned offset = 0; offset < mo->size; offset++)
          os->write8(offset, ((unsigned char *)addr)[offset]);
      }
    } else {
      LLVM_TYPE_Q Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = memory->allocate(size, false, true, &*i);
      if (!mo)
        llvm::report_fatal_error("out of memory");
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      if (!i->hasInitializer())
        os->initializeToRandom();
    }
  }

  // link aliases to their definitions (if bound)
  for (Module::alias_iterator i = m->alias_begin(), ie = m->alias_end();
       i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions.
    globalAddresses.insert(std::make_pair(i, evalConstant(i->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  for (Module::const_global_iterator i = m->global_begin(), e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      MemoryObject *mo = globalObjects.find(i)->second;
      const ObjectState *os = state.addressSpace.findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace.getWriteable(mo, os);

      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      // if(i->isConstant()) os->setReadOnly(true);
    }
  }
}

void Executor::branch(ExecutionState &state,
                      const std::vector<ref<Expr> > &conditions,
                      std::vector<ExecutionState *> &result) {
  TimerStatIncrementer timer(stats::forkTime);
  unsigned N = conditions.size();
  assert(N);

  if (MaxForks != ~0u && stats::forks >= MaxForks) {
    unsigned next = theRNG.getInt32() % N;
    for (unsigned i = 0; i < N; ++i) {
      if (i == next) {
        result.push_back(&state);
      } else {
        result.push_back(NULL);
      }
    }
  } else {
    stats::forks += N - 1;

    // XXX do proper balance or keep random?
    result.push_back(&state);
    for (unsigned i = 1; i < N; ++i) {
      ExecutionState *es = result[theRNG.getInt32() % i];
      ExecutionState *ns = es->branch();
      addedStates.push_back(ns);
      result.push_back(ns);
      es->ptreeNode->data = 0;
      std::pair<PTree::Node *, PTree::Node *> res =
          processTree->split(es->ptreeNode, ns, es);
      ns->ptreeNode = res.first;
      es->ptreeNode = res.second;

      if (INTERPOLATION_ENABLED) {
        if (DebugTracerX)
          llvm::errs() << "[branch:split] Node:" << es->txTreeNode->getNodeSequenceNumber() << " -> Node:";
        std::pair<TxTreeNode *, TxTreeNode *> ires =
            txTree->split(es->txTreeNode, ns, es);
        ns->txTreeNode = ires.first;
        es->txTreeNode = ires.second;
        if (DebugTracerX)
          llvm:: errs() << ires.first->getNodeSequenceNumber() << ", Node:" << ires.second->getNodeSequenceNumber() << "\n";
      }
    }
  }

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).

  std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
                                         siie = seeds.end();
         siit != siie; ++siit) {
      unsigned i;
      for (i = 0; i < N; ++i) {
        ref<ConstantExpr> res;
        bool success = solver->getValue(
            state, siit->assignment.evaluate(conditions[i]), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void)success;
        if (res->isTrue())
          break;
      }

      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i == N)
        i = theRNG.getInt32() % N;

      // Extra check in case we're replaying seeds with a max-fork
      if (result[i])
        seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i = 0; i < N; ++i) {
        if (result[i] && !seedMap.count(result[i])) {
          terminateState(*result[i]);
          result[i] = NULL;
        }
      }
    }
  }

  for (unsigned i = 0; i < N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair Executor::fork(ExecutionState &current, ref<Expr> condition,
                                   bool isInternal) {
  Solver::Validity res;
  std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  if (!isSeeding && !isa<ConstantExpr>(condition) &&
      (MaxStaticForkPct != 1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct != 1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack.back().callPathNode;
    if ((MaxStaticForkPct < 1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) >
             stats::forks * MaxStaticForkPct) ||
        (MaxStaticCPForkPct < 1. && cpn &&
         (cpn->statistics.getValue(stats::forks) >
          stats::forks * MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct < 1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) >
             stats::solverTime * MaxStaticSolvePct) ||
        (MaxStaticCPForkPct < 1. && cpn &&
         (cpn->statistics.getValue(stats::solverTime) >
          stats::solverTime * MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value;
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }
  }

  double timeout = coreSolverTimeout;
  if (isSeeding)
    timeout *= it->second.size();

  // llvm::errs() << "Calling solver->evaluate on query:\n";
  // ExprPPrinter::printQuery(llvm::errs(), current.constraints, condition);

  solver->setTimeout(timeout);
  std::vector<ref<Expr> > unsatCore;
  bool success = solver->evaluate(current, condition, res, unsatCore);
  solver->setTimeout(0);

  if (!success) {
    current.pc = current.prevPC;
    terminateStateEarly(current, "Query timed out (fork).");
    return StatePair(0, 0);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition < replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];

      if (res == Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res == Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if (branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res == Solver::Unknown) {
      assert(!replayKTest && "in replay mode, only one branch can be true.");

      if ((MaxMemoryInhibit && atMemoryLimit) || current.forkDisabled ||
          inhibitForking || (MaxForks != ~0u && stats::forks >= MaxForks)) {

        if (MaxMemoryInhibit && atMemoryLimit)
          klee_warning_once(0, "skipping fork (memory cap exceeded)");
        else if (current.forkDisabled)
          klee_warning_once(0, "skipping fork (fork disabled on current path)");
        else if (inhibitForking)
          klee_warning_once(0, "skipping fork (fork disabled globally)");
        else
          klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && (current.forkDisabled || OnlyReplaySeeds) &&
      res == Solver::Unknown) {
    bool trueSeed = false, falseSeed = false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                                         siie = it->second.end();
         siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success =
          solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);

      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current,
                    trueSeed ? condition : Expr::createIsZero(condition));
    }
  }

  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res == Solver::True) {

    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Validity proof succeeded of a query: antecedent -> consequent.
      // We then extract the unsatisfiability core of antecedent and not
      // consequent as the Craig interpolant.
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[fork:markPathCondition] branch=False, Node:" << current.txTreeNode->getNodeSequenceNumber() << "\n";
    }

    return StatePair(&current, 0);
  } else if (res == Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Falsity proof succeeded of a query: antecedent -> consequent,
      // which means that antecedent -> not(consequent) is valid. In this
      // case also we extract the unsat core of the proof
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[fork:markPathCondition] branch=True, Node:" << current.txTreeNode->getNodeSequenceNumber() << "\n";
    }

    return StatePair(0, &current);
  } else {
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.push_back(falseState);

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
                                           siie = seeds.end();
           siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = solver->getValue(
            current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void)success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }

      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState)
          swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState)
          swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node *, PTree::Node *> res =
        processTree->split(current.ptreeNode, falseState, trueState);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    if (INTERPOLATION_ENABLED) {
      std::pair<TxTreeNode *, TxTreeNode *> ires =
          txTree->split(current.txTreeNode, falseState, trueState);
      falseState->txTreeNode = ires.first;
      trueState->txTreeNode = ires.second;
      if (DebugTracerX)
        llvm::errs() << "[fork:split] branch=Unknown, Node:" << current.txTreeNode->getNodeSequenceNumber()
                     << " -> " << ires.first->getNodeSequenceNumber() << " : " << ires.second->getNodeSequenceNumber() << "\n";
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth <= trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded.");
      terminateStateEarly(*falseState, "max-depth exceeded.");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

std::set<std::string> Executor::extractVarNames(ExecutionState &current,
                                                llvm::Value *v) {
  std::set<std::string> res;
  if (isa<GlobalVariable>(v)) {
    GlobalVariable *gv = cast<GlobalVariable>(v);
    res.insert(gv->getName().data());
  } else if (isa<Instruction>(v)) {
    Instruction *ins = dyn_cast<Instruction>(v);
    switch (ins->getOpcode()) {
    case Instruction::Alloca: {
      AllocaInst *ai = cast<AllocaInst>(ins);
      if (ai->getName() == "") {
        llvm::Function *f = ai->getParent()->getParent();
        if (ai == &f->getEntryBlock().front()) {
          res.insert(f->arg_begin()->getName().data());
        } else if (ai == f->getEntryBlock().front().getNextNode()) {
          res.insert(f->arg_begin()->getNextNode()->getName().data());
        }
      } else {
        res.insert(ai->getName().data());
      }
      break;
    }
    default: {
      for (unsigned i = 0u; i < ins->getNumOperands(); i++) {
        std::set<std::string> tmp =
            extractVarNames(current, ins->getOperand(i));
        res.insert(tmp.begin(), tmp.end());
      }
    }
    }
  }
  return res;
}

Executor::StatePair Executor::branchFork(ExecutionState &current,
                                         ref<Expr> condition, bool isInternal) {
  start = clock();
  // The current node is in the speculation node
  if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
      txTree->isSpeculationNode()) {
    if (SpecStrategyToUse != TIMID) {
      Executor::StatePair res = speculationFork(current, condition, isInternal);
      end = clock();
      txTree->incSpecTime(double(end - start));
      return res;
    }
  }

  Solver::Validity res;
  std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  if (!isSeeding && !isa<ConstantExpr>(condition) &&
      (MaxStaticForkPct != 1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct != 1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack.back().callPathNode;
    if ((MaxStaticForkPct < 1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) >
             stats::forks * MaxStaticForkPct) ||
        (MaxStaticCPForkPct < 1. && cpn &&
         (cpn->statistics.getValue(stats::forks) >
          stats::forks * MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct < 1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) >
             stats::solverTime * MaxStaticSolvePct) ||
        (MaxStaticCPForkPct < 1. && cpn &&
         (cpn->statistics.getValue(stats::solverTime) >
          stats::solverTime * MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value;
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }
  }

  double timeout = coreSolverTimeout;
  if (isSeeding)
    timeout *= it->second.size();

  // llvm::errs() << "Calling solver->evaluate on query:\n";
  // ExprPPrinter::printQuery(llvm::errs(), current.constraints, condition);

  solver->setTimeout(timeout);
  std::vector<ref<Expr> > unsatCore;
  bool success = solver->evaluate(current, condition, res, unsatCore);
  solver->setTimeout(0);

  if (!success) {
    current.pc = current.prevPC;
    terminateStateEarly(current, "Query timed out (fork).");
    return StatePair(0, 0);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition < replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];

      if (res == Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res == Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if (branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res == Solver::Unknown) {
      assert(!replayKTest && "in replay mode, only one branch can be true.");

      if ((MaxMemoryInhibit && atMemoryLimit) || current.forkDisabled ||
          inhibitForking || (MaxForks != ~0u && stats::forks >= MaxForks)) {

        if (MaxMemoryInhibit && atMemoryLimit)
          klee_warning_once(0, "skipping fork (memory cap exceeded)");
        else if (current.forkDisabled)
          klee_warning_once(0, "skipping fork (fork disabled on current path)");
        else if (inhibitForking)
          klee_warning_once(0, "skipping fork (fork disabled globally)");
        else
          klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && (current.forkDisabled || OnlyReplaySeeds) &&
      res == Solver::Unknown) {
    bool trueSeed = false, falseSeed = false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                                         siie = it->second.end();
         siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success =
          solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);

      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current,
                    trueSeed ? condition : Expr::createIsZero(condition));
    }
  }

  if (condition->isTrue()) {
    // do speculation if SpecTypeToUse is SAFETY or COVERAGE
    // the default is NO_SPEC
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      llvm::BranchInst *binst =
          llvm::dyn_cast<llvm::BranchInst>(current.prevPC->inst);
      llvm::BasicBlock *curBB = current.txTreeNode->getBasicBlock();

      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          StatsTracker::increaseEle(curBB, 0, true);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {
          // open speculation & result may be success or fail and Now second
          // check
          if (specSnap[binst] != visitedBlocks.size()) {
            dynamicYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          } else {
            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(&current, 0);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
          } else {
            independenceNo++;
            StatsTracker::increaseEle(curBB, 1, true);
          }
          return StatePair(&current, 0);
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // check independency
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(&current, 0);
          } else {
            // open speculation & result may be success or fail
            independenceNo++;
            StatsTracker::increaseEle(curBB, 0, true);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          }
        } else if (SpecStrategyToUse == CUSTOM) {
          // check independency
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            //          StatsTracker::increaseEle(curBB, 0, true);
            //          StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(&current, 0);
          } else {
            // open speculation & result may be success or fail and Now second
            // check
            independenceNo++;
            if (specSnap[binst] != visitedBlocks.size()) {
              dynamicYes++;
              StatsTracker::increaseEle(curBB, 0, true);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        true);
            } else {
              dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(&current, 0);
            }
          }
        }
      }
    }
  } else if (condition->isFalse()) {
    // do speculation if SpecTypeToUse is SAFETY or COVERAGE
    // the default is NO_SPEC
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      llvm::BranchInst *binst =
          llvm::dyn_cast<llvm::BranchInst>(current.prevPC->inst);
      llvm::BasicBlock *curBB = current.txTreeNode->getBasicBlock();

      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          StatsTracker::increaseEle(curBB, 0, true);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          // open speculation & result may be success or fail and Now second
          // check
          if (specSnap[binst] != visitedBlocks.size()) {
            dynamicYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          } else {
            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(0, &current);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
          } else {
            independenceNo++;
            StatsTracker::increaseEle(curBB, 1, true);
          }
          return StatePair(0, &current);
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(0, &current);
          } else {
            // open speculation & result may be success or fail
            independenceNo++;
            StatsTracker::increaseEle(curBB, 0, true);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          }
        } else if (SpecStrategyToUse == CUSTOM) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            //          StatsTracker::increaseEle(curBB, 0, true);
            //          StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(0, &current);
          } else {
            independenceNo++;
            // open speculation & result may be success or fail and Now second
            // check
            if (specSnap[binst] != visitedBlocks.size()) {
              dynamicYes++;
              StatsTracker::increaseEle(curBB, 0, true);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        false);
            } else {
              dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(0, &current);
            }
          }
        }
      }
    }
  }

  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res == Solver::True) {

    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }
    // do speculation if SpecTypeToUse is SAFETY or COVERAGE
    // the default is NO_SPEC
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      llvm::BranchInst *binst =
          llvm::dyn_cast<llvm::BranchInst>(current.prevPC->inst);
      llvm::BasicBlock *curBB = current.txTreeNode->getBasicBlock();

      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // save unsat core
          // open speculation & result may be success or fail
          StatsTracker::increaseEle(curBB, 0, true);
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {
          // save unsat core
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            dynamicYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          } else {
            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(&current, 0);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(&current, 0);
          } else {
            // marking
            independenceNo++;
            StatsTracker::increaseEle(curBB, 1, true);
            txTree->markPathCondition(current, unsatCore);
            return StatePair(&current, 0);
          }
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(&current, 0);
          } else {
            // save unsat core
            // open speculation & result may be success or fail
            independenceNo++;
            StatsTracker::increaseEle(curBB, 0, true);
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          }
        } else if (SpecStrategyToUse == CUSTOM) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            //          StatsTracker::increaseEle(curBB, 0, true);
            //          StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(&current, 0);
          } else {
            independenceNo++;
            // save unsat core
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              dynamicYes++;
              StatsTracker::increaseEle(curBB, 0, true);
              txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        true);
            } else {
              dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(&current, 0);
            }
          }
        }
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Validity proof succeeded of a query: antecedent -> consequent.
      // We then extract the unsatisfiability core of antecedent and not
      // consequent as the Craig interpolant.
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[branchFork:markPathCondition] res=True, Node:" << current.txTreeNode->getNodeSequenceNumber() << "\n";
      if (WPInterpolant)
        txTree->markInstruction(current.prevPC, true);
    }

    return StatePair(&current, 0);

  } else if (res == Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }
    // do speculation if SpecTypeToUse is SAFETY or COVERAGE
    // the default is NO_SPEC
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      llvm::BranchInst *binst =
          llvm::dyn_cast<llvm::BranchInst>(current.prevPC->inst);
      llvm::BasicBlock *curBB = current.txTreeNode->getBasicBlock();

      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // save unsat core
          // open speculation & result may be success or fail
          StatsTracker::increaseEle(curBB, 0, true);
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          // save unsat core
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            dynamicYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          } else {
            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(0, &current);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(0, &current);
          } else {
            // marking
            independenceNo++;
            StatsTracker::increaseEle(curBB, 1, true);
            txTree->markPathCondition(current, unsatCore);
            return StatePair(0, &current);
          }
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            StatsTracker::increaseEle(curBB, 0, true);
            StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(0, &current);
          } else {
            // save unsat core
            // open speculation & result may be success or fail
            independenceNo++;
            StatsTracker::increaseEle(curBB, 0, true);
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          }
        } else if (SpecStrategyToUse == CUSTOM) {

          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            independenceYes++;
            //          StatsTracker::increaseEle(curBB, 0, true);
            //          StatsTracker::increaseEle(curBB, 2, false);
            return StatePair(0, &current);
          } else {
            independenceNo++;
            // save unsat core
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              dynamicYes++;
              StatsTracker::increaseEle(curBB, 0, true);
              txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        false);
            } else {
              dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(0, &current);
            }
          }
        }
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Falsity proof succeeded of a query: antecedent -> consequent,
      // which means that antecedent -> not(consequent) is valid. In this
      // case also we extract the unsat core of the proof
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[branchFork:markPathCondition] res=False, Node:" << current.txTreeNode->getNodeSequenceNumber() << "\n";
      if (WPInterpolant)
        txTree->markInstruction(current.prevPC, false);
    }

    return StatePair(0, &current);
  } else {

    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.push_back(falseState);

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(),
                                           siie = seeds.end();
           siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = solver->getValue(
            current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void)success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }

      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState)
          swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState)
          swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node *, PTree::Node *> res =
        processTree->split(current.ptreeNode, falseState, trueState);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    if (INTERPOLATION_ENABLED) {
      std::pair<TxTreeNode *, TxTreeNode *> ires =
          txTree->split(current.txTreeNode, falseState, trueState);
      falseState->txTreeNode = ires.first;
      trueState->txTreeNode = ires.second;
      if (DebugTracerX)
        llvm::errs() << "[branchFork:markPathCondition] branch=Unknown, Node:" << current.txTreeNode->getNodeSequenceNumber()
                     << " -> " << ires.first->getNodeSequenceNumber() << " : " << ires.second->getNodeSequenceNumber() << "\n";
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth <= trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded.");
      terminateStateEarly(*falseState, "max-depth exceeded.");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

Executor::StatePair Executor::addSpeculationNode(ExecutionState &current,
                                                 ref<Expr> condition,
                                                 llvm::Instruction *binst,
                                                 bool isInternal,
                                                 bool falseBranchIsInfeasible) {
  current.txTreeNode->secondCheckInst = binst;
  if (falseBranchIsInfeasible == true) {
    // At this point the speculation node should be created and
    // added to the working list in a way that its speculated
    // after the other node is traversed.

    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *trueState, *speculationFalseState = &current;

    ++stats::forks;

    trueState = speculationFalseState->branch();
    addedStates.push_back(trueState);

    current.ptreeNode->data = 0;
    std::pair<PTree::Node *, PTree::Node *> res =
        processTree->split(current.ptreeNode, speculationFalseState, trueState);
    speculationFalseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        speculationFalseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        speculationFalseState->pathOS << "0";
      }
      if (symPathWriter) {
        speculationFalseState->symPathOS =
            symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        speculationFalseState->symPathOS << "0";
      }
    }

    bool isCurrentSpec = current.txTreeNode->isSpeculationNode();
    std::pair<TxTreeNode *, TxTreeNode *> ires =
        txTree->split(current.txTreeNode, speculationFalseState, trueState);
    speculationFalseState->txTreeNode = ires.first;
    speculationFalseState->txTreeNode->setSpeculationFlag();
    if (!isCurrentSpec) {
      speculationFalseState->txTreeNode->visitedProgramPoints =
          new std::set<uintptr_t>();
      speculationFalseState->txTreeNode->specTime = new double(0.0);
    }
    trueState->txTreeNode = ires.second;

    if (!condition->isTrue() && !condition->isFalse()) {
      addConstraint(*trueState, condition);
    }

    return StatePair(trueState, speculationFalseState);
  } else {
    // At this point the speculation node should be created and
    // added to the working list in a way that its speculated
    // after the other node is traversed.

    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *speculationTrueState = &current, *falseState;

    ++stats::forks;

    falseState = speculationTrueState->branch();
    addedStates.push_back(falseState);

    current.ptreeNode->data = 0;
    std::pair<PTree::Node *, PTree::Node *> res =
        processTree->split(current.ptreeNode, speculationTrueState, falseState);
    speculationTrueState->ptreeNode = res.first;
    falseState->ptreeNode = res.second;

    if (!isInternal) {
      if (pathWriter) {
        speculationTrueState->pathOS = pathWriter->open(current.pathOS);
        speculationTrueState->pathOS << "1";
        falseState->pathOS << "0";
      }
      if (symPathWriter) {
        speculationTrueState->symPathOS =
            symPathWriter->open(current.symPathOS);
        speculationTrueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }
    bool isCurrentSpec = current.txTreeNode->isSpeculationNode();
    std::pair<TxTreeNode *, TxTreeNode *> ires =
        txTree->split(current.txTreeNode, speculationTrueState, falseState);
    speculationTrueState->txTreeNode = ires.first;
    speculationTrueState->txTreeNode->setSpeculationFlag();

    if (!isCurrentSpec) {
      speculationTrueState->txTreeNode->visitedProgramPoints =
          new std::set<uintptr_t>();
      speculationTrueState->txTreeNode->specTime = new double(0.0);
    }

    falseState->txTreeNode = ires.second;

    if (!condition->isTrue() && !condition->isFalse()) {
      addConstraint(*falseState, Expr::createIsZero(condition));
    }

    return StatePair(speculationTrueState, falseState);
  }
}

Executor::StatePair Executor::speculationFork(ExecutionState &current,
                                              ref<Expr> condition,
                                              bool isInternal) {

  // Anayzing Speculation node
  // Seeding is removed intentionally
  Solver::Validity res;

  double timeout = coreSolverTimeout;

  // llvm::errs() << "Calling solver->evaluate on query:\n";
  // ExprPPrinter::printQuery(llvm::errs(), current.constraints, condition);

  solver->setTimeout(timeout);
  std::vector<ref<Expr> > unsatCore;
  bool success = solver->evaluate(current, condition, res, unsatCore);
  solver->setTimeout(0);

  if (!success) {
    current.pc = current.prevPC;
    terminateStateEarly(current, "Query timed out (fork).");
    return StatePair(0, 0);
  }

  llvm::BranchInst *binst =
      llvm::dyn_cast<llvm::BranchInst>(current.prevPC->inst);

  if (condition->isTrue()) {
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {

      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            //            dynamicYes++;
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          } else {
            //            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(&current, 0);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          klee_error(
              "SPECULATION: timid strategy never runs in speculationFork!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {

          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            //          independenceYes++;
            return StatePair(&current, 0);
          } else {
            //          independenceNo++;
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              //            dynamicYes++;
              return addSpeculationNode(current, condition, binst, isInternal,
                                        true);
            } else {
              //            dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(&current, 0);
            }
          }
        }
      }
    }
    return StatePair(&current, 0);
  } else if (condition->isFalse()) {
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            //            dynamicYes++;
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          } else {
            //            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(0, &current);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          klee_error(
              "SPECULATION: timid strategy never runs in speculationFork!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          // open speculation & result may be success or fail
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            //          independenceYes++;
            return StatePair(0, &current);
          } else {
            //          independenceNo++;
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              //            dynamicYes++;
              return addSpeculationNode(current, condition, binst, isInternal,
                                        false);
            } else {
              //            dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(0, &current);
            }
          }
        }
      }
    }
    return StatePair(0, &current);
  }
  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res == Solver::True) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }
    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      if (SpecStrategyToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {
          // save unsat core
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            //            dynamicYes++;
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      true);
          } else {
            //            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(&current, 0);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          klee_error(
              "SPECULATION: timid strategy never runs in speculationFork!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    true);
        } else if (SpecStrategyToUse == CUSTOM) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            //          independenceYes++;
            return StatePair(&current, 0);
          } else {
            //          independenceNo++;
            // save unsat core
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              //            dynamicYes++;
              txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        true);
            } else {
              //            dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(&current, 0);
            }
          }
        }
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Validity proof succeeded of a query: antecedent -> consequent.
      // We then extract the unsatisfiability core of antecedent and not
      // consequent as the Craig interpolant.
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[speculationFork:markPathCondition] branch=True, Node:" << current.txTreeNode->getNodeSequenceNumber() <<"\n";
      if (WPInterpolant)
        txTree->markInstruction(current.prevPC, true);
    }

    return StatePair(&current, 0);

    //    txTree->markPathCondition(current, unsatCore);
    //    return StatePair(&current, 0);
  } else if (res == Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
        TxSpeculationHelper::isStateSpeculable(current)) {
      if (SpecTypeToUse == SAFETY) {
        if (SpecStrategyToUse == TIMID) {
          klee_error("SPECULATION: timid is not supported with safety!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          // save unsat core
          // open speculation & result may be success or fail
          if (specSnap[binst] != visitedBlocks.size()) {
            //            dynamicYes++;
            txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
            return addSpeculationNode(current, condition, binst, isInternal,
                                      false);
          } else {
            //            dynamicNo++;
            // then close speculation & do marking as deletion
            txTree->markPathCondition(current, unsatCore);
            return StatePair(0, &current);
          }
        }
      } else {
        if (SpecStrategyToUse == TIMID) {
          klee_error(
              "SPECULATION: timid strategy never runs in speculationFork!");
        } else if (SpecStrategyToUse == AGGRESSIVE) {
          txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
          return addSpeculationNode(current, condition, binst, isInternal,
                                    false);
        } else if (SpecStrategyToUse == CUSTOM) {
          std::set<std::string> vars = extractVarNames(current, binst);
          if (TxSpeculationHelper::isIndependent(vars, bbOrderToSpecAvoid)) {
            // open speculation & assume success
            //          independenceYes++;
            return StatePair(0, &current);
          } else {
            //          independenceNo++;
            // save unsat core
            // open speculation & result may be success or fail
            if (specSnap[binst] != visitedBlocks.size()) {
              //            dynamicYes++;
              txTree->storeSpeculationUnsatCore(solver, unsatCore, binst);
              return addSpeculationNode(current, condition, binst, isInternal,
                                        false);
            } else {
              //            dynamicNo++;
              // then close speculation & do marking as deletion
              txTree->markPathCondition(current, unsatCore);
              return StatePair(0, &current);
            }
          }
        }
      }
    }

    if (INTERPOLATION_ENABLED) {
      // Falsity proof succeeded of a query: antecedent -> consequent,
      // which means that antecedent -> not(consequent) is valid. In this
      // case also we extract the unsat core of the proof
      txTree->markPathCondition(current, unsatCore);
      if (DebugTracerX)
        llvm::errs() << "[speculationFork:markPathCondition] branch=False, Node:" << current.txTreeNode->getNodeSequenceNumber() <<"\n";
      if (WPInterpolant)
        txTree->markInstruction(current.prevPC, false);
    }

    return StatePair(0, &current);

    //    txTree->markPathCondition(current, unsatCore);
    //    return StatePair(0, &current);
  } else {
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;

    falseState = trueState->branch();
    addedStates.push_back(falseState);

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    current.ptreeNode->data = 0;
    std::pair<PTree::Node *, PTree::Node *> resNode =
        processTree->split(current.ptreeNode, falseState, trueState);
    falseState->ptreeNode = resNode.first;
    trueState->ptreeNode = resNode.second;

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    std::pair<TxTreeNode *, TxTreeNode *> ires =
        txTree->split(current.txTreeNode, falseState, trueState);

    falseState->txTreeNode = ires.first;
    trueState->txTreeNode = ires.second;
    if (DebugTracerX)
      llvm::errs() << "[speculationFork:split] branch=Unknown, Node:" << current.txTreeNode->getNodeSequenceNumber()
                   << " -> " << ires.first->getNodeSequenceNumber() << " : " << ires.second->getNodeSequenceNumber() << "\n";


    if (res != Solver::False)
      addConstraint(*trueState, condition);
    if (res != Solver::True)
      addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth <= trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded.");
      terminateStateEarly(*falseState, "max-depth exceeded.");
      return StatePair(0, 0);
    }

    return StatePair(trueState, falseState);
  }
}

void Executor::speculativeBackJump(ExecutionState &current) {

  double thisSpecTreeTime = *(current.txTreeNode->specTime);
  // identify the speculation root
  TxTreeNode *currentNode = current.txTreeNode;
  TxTreeNode *parent = currentNode->getParent();
  while (parent && parent->isSpeculationNode()) {
    currentNode = parent;
    parent = parent->getParent();
  }

  StatsTracker::increaseEle(parent->getBasicBlock(), 1, false);

  // interpolant marking on parent node
  if (parent && !parent->speculationUnsatCore.empty()) {
    parent->mark();
  }
  specSnap[parent->secondCheckInst] = visitedBlocks.size();

  // collect & mark speculation fail all nodes in the sub tree
  std::vector<TxTreeNode *> deletedNodes = collectSpeculationNodes(currentNode);

  // collect removed states which pointing to speculation fail node
  std::vector<ExecutionState *> removedSpeculationStates;
  for (std::set<ExecutionState *>::const_iterator it = states.begin(),
                                                  ie = states.end();
       it != ie; ++it) {
    ExecutionState *tmp = (*it);
    if (tmp->txTreeNode->isSpeculationFailedNode()) {
      removedSpeculationStates.push_back(tmp);
    }
  }

  // update states in search
  searcher->update(0, std::vector<ExecutionState *>(),
                   removedSpeculationStates);
  // remove fail nodes in subtree
  for (std::vector<TxTreeNode *>::iterator it = deletedNodes.begin(),
                                           ie = deletedNodes.end();
       it != ie; ++it) {
    txTree->removeSpeculationFailedNodes(*it);
  }
  // remove state in states
  for (std::vector<ExecutionState *>::iterator
           it = removedSpeculationStates.begin(),
           ie = removedSpeculationStates.end();
       it != ie; ++it) {
    states.erase(*it);
    if (&current != *it)
      delete *it;
  }
  // this count is for the fail node in spec tree
  end = clock();
  thisSpecTreeTime += (double(end - start));

  // add fail time for spec subtree
  totalSpecFailTime += thisSpecTreeTime;
}

std::vector<TxTreeNode *> Executor::collectSpeculationNodes(TxTreeNode *root) {
  if (!root)
    return std::vector<TxTreeNode *>();
  std::vector<TxTreeNode *> leftNodes =
      collectSpeculationNodes(root->getLeft());
  std::vector<TxTreeNode *> rightNodes =
      collectSpeculationNodes(root->getRight());
  std::vector<TxTreeNode *> result;
  result.insert(result.end(), leftNodes.begin(), leftNodes.end());
  result.insert(result.end(), rightNodes.begin(), rightNodes.end());
  // mark root fail & add to result
  root->setSpeculationFailed();
  result.insert(result.end(), root);
  return result;
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    if (!CE->isTrue())
      llvm::report_fatal_error("attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                                         siie = it->second.end();
         siit != siie; ++siit) {
      bool res;
      bool success =
          solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint");
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition,
                                 ConstantExpr::alloc(1, Expr::Bool));
}

ref<klee::ConstantExpr> Executor::evalConstant(const Constant *c) {
  if (const llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
    return evalConstantExpr(ce);
  } else {
    if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
      return ConstantExpr::alloc(ci->getValue());
    } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {
      return ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt());
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      return globalAddresses.find(gv)->second;
    } else if (isa<ConstantPointerNull>(c)) {
      return Expr::createPointer(0);
    } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
      return ConstantExpr::create(0, getWidthForLLVMType(c->getType()));
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
    } else if (const ConstantDataSequential *cds =
                   dyn_cast<ConstantDataSequential>(c)) {
      std::vector<ref<Expr> > kids;
      for (unsigned i = 0, e = cds->getNumElements(); i != e; ++i) {
        ref<Expr> kid = evalConstant(cds->getElementAsConstant(i));
        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
#endif
    } else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
      const StructLayout *sl =
          kmodule->targetData->getStructLayout(cs->getType());
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = cs->getNumOperands(); i != 0; --i) {
        unsigned op = i - 1;
        ref<Expr> kid = evalConstant(cs->getOperand(op));

        uint64_t thisOffset = sl->getElementOffsetInBits(op),
                 nextOffset = (op == cs->getNumOperands() - 1)
                                  ? sl->getSizeInBits()
                                  : sl->getElementOffsetInBits(op + 1);
        if (nextOffset - thisOffset > kid->getWidth()) {
          uint64_t paddingWidth = nextOffset - thisOffset - kid->getWidth();
          kids.push_back(ConstantExpr::create(0, paddingWidth));
        }

        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else if (const ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
      llvm::SmallVector<ref<Expr>, 4> kids;
      for (unsigned i = ca->getNumOperands(); i != 0; --i) {
        unsigned op = i - 1;
        ref<Expr> kid = evalConstant(ca->getOperand(op));
        kids.push_back(kid);
      }
      ref<Expr> res = ConcatExpr::createN(kids.size(), kids.data());
      return cast<ConstantExpr>(res);
    } else {
      // Constant{Vector}
      llvm::report_fatal_error("invalid argument to evalConstant()");
    }
  }
}

const Cell &Executor::eval(KInstruction *ki, unsigned index,
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack.back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state,
                         ref<Expr> value) {
  getDestCell(state, target).value = value;
}

void Executor::bindArgument(KFunction *kf, unsigned index,
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;

    solver->setTimeout(coreSolverTimeout);
    if (solver->getValue(state, e, value) &&
        solver->mustBeTrue(state, EqExpr::create(e, value), isTrue) && isTrue)
      result = value;
    solver->setTimeout(0);
  }

  return result;
}

/* Concretize the given expression, and return a possible constant value.
   'reason' is just a documentation string stating the reason for
   concretization. */
ref<klee::ConstantExpr> Executor::toConstant(ExecutionState &state, ref<Expr> e,
                                             const char *reason) {
  e = state.constraints.simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void)success;

  std::string str;
  llvm::raw_string_ostream os(str);
  os << "silently concretizing (reason: " << reason << ") expression " << e
     << " to value " << value << " (" << (*(state.pc)).info->file << ":"
     << (*(state.pc)).info->line << ")";

  if (AllExternalWarnings)
    klee_warning(reason, os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));

  return value;
}

void Executor::executeGetValue(ExecutionState &state, ref<Expr> e,
                               KInstruction *target) {
  e = state.constraints.simplifyExpr(e);
  std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
      seedMap.find(&state);
  if (it == seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, e, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void)success;
    bindLocal(target, state, value);

    if (INTERPOLATION_ENABLED) {
      txTree->execute(target->inst, e, value);
      if (DebugTracerX) {
        llvm::errs() << "[executeGetValue:execute] Node:" << state.txTreeNode->getNodeSequenceNumber()
                     << ", Inst:" << target->inst->getOpcodeName()
                     << ", Value:";
        value->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
  } else {
    std::set<ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                                         siie = it->second.end();
         siit != siie; ++siit) {
      ref<ConstantExpr> value;
      bool success =
          solver->getValue(state, siit->assignment.evaluate(e), value);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      values.insert(value);
    }

    std::vector<ref<Expr> > conditions;
    for (std::set<ref<Expr> >::iterator vit = values.begin(),
                                        vie = values.end();
         vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState *> branches;
    branch(state, conditions, branches);

    std::vector<ExecutionState *>::iterator bit = branches.begin();
    for (std::set<ref<Expr> >::iterator vit = values.begin(),
                                        vie = values.end();
         vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      if (INTERPOLATION_ENABLED) {
        TxTree::executeOnNode(es->txTreeNode, target->inst, e, *vit);
        if (DebugTracerX) {
          llvm::errs() << "[executeGetValue:executeOnNode] Node:" << es->txTreeNode->getNodeSequenceNumber()
                       << ", Inst:" << target->inst->getOpcodeName()
                       << ", Value:";
          (*vit)->print(llvm::errs());
          llvm::errs() << "\n";
        }
      }
      ++bit;
    }
  }
}

void Executor::printDebugInstructions(ExecutionState &state) {
  // check do not print
  if (DebugPrintInstructions.size() == 0)
    return;

  llvm::raw_ostream *stream = 0;
  if (optionIsSet(DebugPrintInstructions, STDERR_ALL) ||
      optionIsSet(DebugPrintInstructions, STDERR_SRC) ||
      optionIsSet(DebugPrintInstructions, STDERR_COMPACT))
    stream = &llvm::errs();
  else
    stream = &debugLogBuffer;

  if (!optionIsSet(DebugPrintInstructions, STDERR_COMPACT) &&
      !optionIsSet(DebugPrintInstructions, FILE_COMPACT))
    printFileLine(state, state.pc, *stream);

  (*stream) << state.pc->info->id;

  if (optionIsSet(DebugPrintInstructions, STDERR_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_ALL))
    (*stream) << ":" << *(state.pc->inst);
  (*stream) << "\n";

  if (optionIsSet(DebugPrintInstructions, FILE_ALL) ||
      optionIsSet(DebugPrintInstructions, FILE_COMPACT) ||
      optionIsSet(DebugPrintInstructions, FILE_SRC)) {
    debugLogBuffer.flush();
    (*debugInstFile) << debugLogBuffer.str();
    debugBufferString = "";
  }
}

void Executor::stepInstruction(ExecutionState &state) {
  printDebugInstructions(state);
  if (statsTracker)
    statsTracker->stepInstruction(state);

  ++stats::instructions;
  state.prevPC = state.pc;
  ++state.pc;

  if (stats::instructions == StopAfterNInstructions)
    haltExecution = true;
}

void Executor::executeCall(ExecutionState &state, KInstruction *ki, Function *f,
                           std::vector<ref<Expr> > &arguments) {
  // BB Coverage
  bool isInterested = (fBBOrder.find(f) != fBBOrder.end());
  if (isInterested) {
    bool isInSpecMode = (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
                         state.txTreeNode->isSpeculationNode());
    processBBCoverage(BBCoverage, &(f->front()), isInSpecMode);
  }

  Instruction *i = ki->inst;
  if (f && f->isDeclaration()) {
    switch (f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;

    // va_arg is handled by caller and intrinsic lowering, see comment for
    // ExecutionState::varargs
    case Intrinsic::vastart: {
      StackFrame &sf = state.stack.back();

      // varargs can be zero if no varargs were provided
      if (!sf.varargs)
        return;

      // FIXME: This is really specific to the architecture, not the
      // pointer size. This happens to work fir x86-32 and x86-64,
      // however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, true, arguments[0],
                               sf.varargs->getBaseExpr(), 0);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // X86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, true, arguments[0],
                               ConstantExpr::create(48, 32), 0); // gp_offset
        executeMemoryOperation(
            state, true,
            AddExpr::create(arguments[0], ConstantExpr::create(4, 64)),
            ConstantExpr::create(304, 32), 0); // fp_offset
        executeMemoryOperation(
            state, true,
            AddExpr::create(arguments[0], ConstantExpr::create(8, 64)),
            sf.varargs->getBaseExpr(), 0); // overflow_arg_area
        executeMemoryOperation(
            state, true,
            AddExpr::create(arguments[0], ConstantExpr::create(16, 64)),
            ConstantExpr::create(0, 64), 0); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with vaeend, however (like call it twice).
      break;

    case Intrinsic::vacopy:
    // va_copy should have been lowered.
    //
    // FIXME: It would be nice to check for errors in the usage of this as
    // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
  } else {
    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    KFunction *kf = kmodule->functionMap[f];
    state.pushFrame(state.prevPC, kf);
    state.pc = kf->instructions;

    if (statsTracker)
      statsTracker->framePushed(state, &state.stack[state.stack.size() - 2]);

    // TODO: support "byval" parameter attribute
    // TODO: support zeroext, signext, sret attributes

    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.",
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }
    } else {
      Expr::Width WordSize = Context::get().getPointerWidth();

      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments",
                              User);
        return;
      }

      StackFrame &sf = state.stack.back();
      unsigned size = 0;
      bool requires16ByteAlignment = false;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work for x86-32 and x86-64, however.
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          Expr::Width argWidth = arguments[i]->getWidth();
          // AMD64-ABI 3.5.7p5: Step 7. Align l->overflow_arg_area upwards to a
          // 16 byte boundary if alignment needed by type exceeds 8 byte
          // boundary.
          //
          // Alignment requirements for scalar types is the same as their size
          if (argWidth > Expr::Int64) {
            size = llvm::RoundUpToAlignment(size, 16);
            requires16ByteAlignment = true;
          }
          size += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
        }
      }

      MemoryObject *mo = sf.varargs =
          memory->allocate(size, true, false, state.prevPC->inst,
                           (requires16ByteAlignment ? 16 : 8));
      if (!mo && size) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }

      if (mo) {
        if ((WordSize == Expr::Int64) && (mo->address & 15) &&
            requires16ByteAlignment) {
          // Both 64bit Linux/Glibc and 64bit MacOSX should align to 16 bytes.
          klee_warning_once(
              0, "While allocating varargs: malloc did not align to 16 bytes.");
        }

        ObjectState *os = bindObjectInState(state, mo, true);
        unsigned offset = 0;
        for (unsigned i = funcArgs; i < callingArgs; i++) {
          // FIXME: This is really specific to the architecture, not the pointer
          // size. This happens to work for x86-32 and x86-64, however.
          if (WordSize == Expr::Int32) {
            os->write(offset, arguments[i]);
            offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
          } else {
            assert(WordSize == Expr::Int64 && "Unknown word size!");

            Expr::Width argWidth = arguments[i]->getWidth();
            if (argWidth > Expr::Int64) {
              offset = llvm::RoundUpToAlignment(offset, 16);
            }
            os->write(offset, arguments[i]);
            offset += llvm::RoundUpToAlignment(argWidth, WordSize) / 8;
          }
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i = 0; i < numFormals; ++i)
      bindArgument(kf, i, state, arguments[i]);

    if (INTERPOLATION_ENABLED) {
      // We bind the abstract dependency call arguments
      state.txTreeNode->bindCallArguments(state.prevPC->inst, arguments);
      if (DebugTracerX)
        llvm::errs() << "[executeCall:bindCallArguments] !f->isDeclaration(), Node:" << state.txTreeNode->getNodeSequenceNumber()
                     << ", inst:" << state.prevPC->inst->getOpcodeName() << "\n";
    }
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src,
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.

  // XXX this lookup has to go ?
  KFunction *kf = state.stack.back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.pc = &kf->instructions[entry];
  if (state.pc->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode *>(state.pc->inst);
    state.incomingBBIndex = first->getBasicBlockIndex(src);
  }
  if (INTERPOLATION_ENABLED) {
    // blockCount increased to count all visited Basic Blocks
    TxTree::blockCount++;
  }

  // process BB Coverage
  bool isInterested = (fBBOrder.find(dst->getParent()) != fBBOrder.end());
  if (isInterested) {

    bool isInSpecMode = (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
                         state.txTreeNode->isSpeculationNode());
    processBBCoverage(BBCoverage, dst, isInSpecMode);
  }
}

void Executor::processBBCoverage(int BBCoverage, llvm::BasicBlock *bb,
                                 bool isInSpecMode) {
  if (BBCoverage >= 1) {
    bool isNew = (visitedBlocks.find(bb) == visitedBlocks.end());
    int order = fBBOrder[bb->getParent()][bb];
    if (!isInSpecMode && isNew) {
      // add to visited BBs if not in speculation mode
      visitedBlocks.insert(bb);
    }
    float percent = ((float)visitedBlocks.size() / (float)allBlockCount) * 100;
    // print percentage if this is a new BB
    if (BBCoverage >= 2 && isNew) {
      // print live %

      std::string livePercentCovFile =
          interpreterHandler->getOutputFilename("LivePercentCov.txt");
      std::ofstream livePercentCovFileOut(livePercentCovFile.c_str(),
                                          std::ofstream::app);
      // [BB order - No. Visited - Total - %]
      livePercentCovFileOut << "[" << visitedBlocks.size() << ","
                            << allBlockCount << "," << percent << "]\n";
      livePercentCovFileOut.close();
    }

    // print live BB
    if (BBCoverage >= 3 && isNew && !isInSpecMode) {
      std::string liveBBFile =
          interpreterHandler->getOutputFilename("LiveBB.txt");
      std::ofstream liveBBFileOut(liveBBFile.c_str(), std::ofstream::app);
      liveBBFileOut << "-- BlockScopeStarts --\n";
      liveBBFileOut << "Function: " << bb->getParent()->getName().str() << "\n";
      liveBBFileOut << "Block Order: " << order;
      // block content
      std::string tmp;
      raw_string_ostream tmpOS(tmp);
      bb->print(tmpOS);
      liveBBFileOut << tmp;
      liveBBFileOut << "-- BlockScopeEnds --\n\n";
      liveBBFileOut.close();
    }
    if (BBCoverage >= 4 && isNew && !isInSpecMode) {
      // Print covered atomic condition covered
      std::string liveBBFileICMP =
          interpreterHandler->getOutputFilename("coveredICMP.txt");
      std::ofstream liveBBFileICMPOut(liveBBFileICMP.c_str(),
                                      std::ofstream::app);

      // block content
      std::string tmpICMP;
      raw_string_ostream tmpICMPOS(tmpICMP);
      for (llvm::BasicBlock::iterator icmp = bb->begin(); icmp != bb->end();
           icmp++) {
        if (llvm::isa<llvm::ICmpInst>(icmp)) {
          coveredICMPCount++;
          liveBBFileICMPOut << "Function: " << bb->getParent()->getName().str()
                            << " ";
          liveBBFileICMPOut << "Block Order: " << order;
          icmp->print(tmpICMPOS);
          liveBBFileICMPOut << tmpICMP << "\n";
        }
      }
      liveBBFileICMPOut.close();
    }
    if (BBCoverage >= 5) {
      double diff = time(0) - startingBBPlottingTime;
      std::string bbPlottingFile =
          interpreterHandler->getOutputFilename("BBPlotting.txt");
      std::ofstream bbPlotingFileOut(bbPlottingFile.c_str(),
                                     std::ofstream::app);
      bbPlotingFileOut << diff << "     " << std::fixed << std::setprecision(2)
                       << percent << "\n";
      bbPlotingFileOut.close();
    }
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki,
                             llvm::raw_ostream &debugFile) {
  const InstructionInfo &ii = *ki->info;
  if (ii.file != "")
    debugFile << "     " << ii.file << ":" << ii.line << ":";
  else
    debugFile << "     [no debug info]:";
}

/// Compute the true target of a function call, resolving LLVM and KLEE
/// aliases and bitcasts.
Function *Executor::getTargetFunction(Value *calledVal, ExecutionState &state) {
  SmallPtrSet<const GlobalValue *, 3> Visited;

  Constant *c = dyn_cast<Constant>(calledVal);
  if (!c)
    return 0;

  while (true) {
    if (GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      if (!Visited.insert(gv))
        return 0;

      std::string alias = state.getFnAlias(gv->getName());
      if (alias != "") {
        llvm::Module *currModule = kmodule->module;
        GlobalValue *old_gv = gv;
        gv = currModule->getNamedValue(alias);
        if (!gv) {
          klee_error("Function %s(), alias for %s not found!\n", alias.c_str(),
                     old_gv->getName().str().c_str());
        }
      }

      if (Function *f = dyn_cast<Function>(gv))
        return f;
      else if (GlobalAlias *ga = dyn_cast<GlobalAlias>(gv))
        c = ga->getAliasee();
      else
        return 0;
    } else if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
      if (ce->getOpcode() == Instruction::BitCast)
        c = ce->getOperand(0);
      else
        return 0;
    } else
      return 0;
  }
}

/// TODO remove?
static bool isDebugIntrinsic(const Function *f, KModule *KM) { return false; }

static inline const llvm::fltSemantics *fpWidthToSemantics(unsigned width) {
  switch (width) {
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
  default:
    return 0;
  }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;
  // if this is starting a new BB then
  // check for non-linear & new BB in speculation mode  
  if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
      txTree->isSpeculationNode() &&
      (i == &state.txTreeNode->getBasicBlock()->front())) {
    // check non-linear
    uintptr_t pp = state.txTreeNode->getProgramPoint();
    bool isPPVisited = (state.txTreeNode->visitedProgramPoints->find(pp) !=
                        state.txTreeNode->visitedProgramPoints->end());
    if (isPPVisited) {
      // add to spec revisited statistic
      if (specRevisited.find(pp) != specRevisited.end()) {
        specRevisited[pp] = specRevisited[pp] + 1;
      } else {
        specRevisited[pp] = 1;
      }
      // check interpolation at is program point
      bool hasInterpolation = TxSubsumptionTable::hasInterpolation(state);
      if (!hasInterpolation) {
        if (specRevisitedNoInter.find(pp) != specRevisitedNoInter.end()) {
          specRevisitedNoInter[pp] = specRevisitedNoInter[pp] + 1;
        } else {
          specRevisitedNoInter[pp] = 1;
        }
      }
      specFail++;
      speculativeBackJump(state);
      return;
    } else {
      // Storing the visited program points.
      state.txTreeNode->visitedProgramPoints->insert(pp);
    }

    // check new BB
    if (SpecTypeToUse == COVERAGE) {
      llvm::BasicBlock *currentBB = state.txTreeNode->getBasicBlock();
      if (visitedBlocks.find(currentBB) == visitedBlocks.end()) {
        if (specFailNew.find(pp) != specFailNew.end()) {
          specFailNew[pp] = specFailNew[pp] + 1;
        } else {
          specFailNew[pp] = 1;
        }
        // check interpolation at is program point
        bool hasInterpolation = TxSubsumptionTable::hasInterpolation(state);
        if (!hasInterpolation) {
          if (specFailNoInter.find(pp) != specFailNoInter.end()) {
            specFailNoInter[pp] = specFailNoInter[pp] + 1;
          } else {
            specFailNoInter[pp] = 1;
          }
        }
        // add to visited BB
        // This is disabled to not to count blocks in speculation subtree
        // visitedBlocks.insert(currentBB);
        specFail++;
        speculativeBackJump(state);
        return;
      }
    }
  }

  if (INTERPOLATION_ENABLED && WPInterpolant)
    txTree->storeInstruction(ki, state.incomingBBIndex);

  switch (i->getOpcode()) {
  // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack.back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);

    if (!isVoidReturn) {
      result = eval(ki, 0, state).value;
    }

    if (state.stack.size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      terminateStateOnExit(state);
    } else {
      state.popFrame(ki, result);

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.pc = kcaller;
        ++state.pc;
      }

      if (!isVoidReturn) {
        LLVM_TYPE_Q Type *t = caller->getType();
        if (t != Type::getVoidTy(getGlobalContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(t);

          if (from != to) {
            CallSite cs =
                (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller))
                                         : CallSite(cast<CallInst>(caller)));

// XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
            bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#elif LLVM_VERSION_CODE >= LLVM_VERSION(3, 2)
            bool isSExt = cs.paramHasAttr(0, llvm::Attributes::SExt);
#else
            bool isSExt = cs.paramHasAttr(0, llvm::Attribute::SExt);
#endif
            if (isSExt) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(
              state, "return void when caller expected a result");
        }
      }
    }
    break;
  }
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 1)
  case Instruction::Unwind: {
    for (;;) {
      KInstruction *kcaller = state.stack.back().caller;
      state.popFrame(ki, ConstantExpr::alloc(0, Expr::Bool));

      if (statsTracker)
        statsTracker->framePopped(state);

      if (state.stack.empty()) {
        terminateStateOnExecError(state, "unwind from initial stack frame");
        break;
      } else {
        Instruction *caller = kcaller->programPoint;
        if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
          transferToBasicBlock(ii->getUnwindDest(), caller->getParent(), state);
          break;
        }
      }
    }
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] Unwind, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
#endif
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    // stop collecting phi values for the current node
    if (INTERPOLATION_ENABLED) {
      txTree->setPhiValuesFlag(0);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:setPhiValuesFlag] Br, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
      if (INTERPOLATION_ENABLED) {
        txTree->execute(i);
        if (DebugTracerX)
          llvm::errs() << "[executeInstruction:execute] Br Unconditional, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) && "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;

      Executor::StatePair branches = branchFork(state, cond, false);

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack.back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(),
                             *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(),
                             *branches.second);

      // Below we test if some of the branches are not available for
      // exploration, which means that there is a dependency of the program
      // state on the control variables in the conditional. Such variables
      // (allocations) need to be marked as belonging to the core.
      // This is mainly to take care of the case when the conditional
      // variables are not marked using unsatisfiability core as the
      // conditional is concrete and therefore there has been no invocation
      // of the solver to decide its satisfiability, and no generation
      // of the unsatisfiability core.
      if (INTERPOLATION_ENABLED && ((!branches.first && branches.second) ||
                                    (branches.first && !branches.second))) {
        txTree->execute(i);
        if (DebugTracerX)
          llvm::errs() << "[executeInstruction:execute] Br, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }
    }
    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    BasicBlock *bb = si->getParent();

    // For interpolation
    ref<Expr> oldCond = cond;

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      LLVM_TYPE_Q llvm::IntegerType *Ty =
          cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
      unsigned index = si->findCaseValue(ci).getSuccessorIndex();
#else
      unsigned index = si->findCaseValue(ci);
#endif
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);

      if (INTERPOLATION_ENABLED) {
        txTree->execute(i, oldCond);
        if (DebugTracerX)
          llvm::errs() << "[executeInstruction:execute] Switch, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }
    } else {
      // Handle possible different branch targets

      // We have the following assumptions:
      // - each case value is mutual exclusive to all other values including the
      //   default value
      // - order of case branches is based on the order of the expressions of
      //   the scase values, still default is handled last
      std::vector<BasicBlock *> bbOrder;
      std::map<BasicBlock *, ref<Expr> > branchTargets;

      std::map<ref<Expr>, BasicBlock *> expressionOrder;

// Iterate through all non-default cases and order them by expressions
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
      for (SwitchInst::CaseIt i = si->case_begin(), e = si->case_end(); i != e;
           ++i) {
        ref<Expr> value = evalConstant(i.getCaseValue());
#else
      for (unsigned i = 1, cases = si->getNumCases(); i < cases; ++i) {
        ref<Expr> value = evalConstant(si->getCaseValue(i));
#endif

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
        BasicBlock *caseSuccessor = i.getCaseSuccessor();
#else
        BasicBlock *caseSuccessor = si->getSuccessor(i);
#endif
        expressionOrder.insert(std::make_pair(value, caseSuccessor));
      }

      // Track default branch values
      ref<Expr> defaultValue = ConstantExpr::alloc(1, Expr::Bool);

      // iterate through all non-default cases but in order of the expressions
      for (std::map<ref<Expr>, BasicBlock *>::iterator
               it = expressionOrder.begin(),
               itE = expressionOrder.end();
           it != itE; ++it) {
        std::vector<ref<Expr> > unsatCore;
        ref<Expr> match = EqExpr::create(cond, it->first);

        // Make sure that the default value does not contain this target's value
        defaultValue = AndExpr::create(defaultValue, Expr::createIsZero(match));

        // Check if control flow could take this case
        bool result;
        bool success = solver->mayBeTrue(state, match, result, unsatCore);
        assert(success && "FIXME: Unhandled solver failure");
        (void)success;
        if (result) {
          BasicBlock *caseSuccessor = it->second;

          // Handle the case that a basic block might be the target of multiple
          // switch cases.
          // Currently we generate an expression containing all switch-case
          // values for the same target basic block. We spare us forking too
          // many times but we generate more complex condition expressions
          // TODO Add option to allow to choose between those behaviors
          std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> res =
              branchTargets.insert(std::make_pair(
                  caseSuccessor, ConstantExpr::alloc(0, Expr::Bool)));

          res.first->second = OrExpr::create(match, res.first->second);

          // Only add basic blocks which have not been target of a branch yet
          if (res.second) {
            bbOrder.push_back(caseSuccessor);
          }
        } else if (INTERPOLATION_ENABLED) {
          // The solver returned no solution, which means there is an infeasible
          // branch: Mark the unsatisfiability core
          state.txTreeNode->unsatCoreInterpolation(unsatCore);
          if (DebugTracerX)
            llvm::errs() << "[executeInstruction:unsatCoreInterpolation] Switch, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
        }
      }

      // Check if control could take the default case
      std::vector<ref<Expr> > unsatCore;
      bool res;
      bool success = solver->mayBeTrue(state, defaultValue, res, unsatCore);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (res) {
        std::pair<std::map<BasicBlock *, ref<Expr> >::iterator, bool> ret =
            branchTargets.insert(
                std::make_pair(si->getDefaultDest(), defaultValue));
        if (ret.second) {
          bbOrder.push_back(si->getDefaultDest());
        }
      } else if (INTERPOLATION_ENABLED) {
        // The solver returned no solution, which means the default branch
        // cannot be taken: Mark the unsatisfiability core
        state.txTreeNode->unsatCoreInterpolation(unsatCore);
        if (DebugTracerX)
          llvm::errs() << "[executeInstruction:unsatCoreInterpolation] Switch, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }

      // Fork the current state with each state having one of the possible
      // successors of this switch
      std::vector<ref<Expr> > conditions;
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        conditions.push_back(branchTargets[*it]);
      }
      std::vector<ExecutionState *> branches;
      branch(state, conditions, branches);

      std::vector<ExecutionState *>::iterator bit = branches.begin();
      for (std::vector<BasicBlock *>::iterator it = bbOrder.begin(),
                                               ie = bbOrder.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es)
          transferToBasicBlock(*it, bb, *es);
        ++bit;
      }
    }
    break;
  }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    CallSite cs(i);
    unsigned numArgs = cs.arg_size();
    Value *fp = cs.getCalledValue();
    Function *f = getTargetFunction(fp, state);

    // Skip debug intrinsics, we can't evaluate their metadata arguments.
    if (f && isDebugIntrinsic(f, kmodule))
      break;

    if (isa<InlineAsm>(fp)) {
      terminateStateOnExecError(state, "inline assembly is unsupported");
      break;
    }
    // evaluate arguments
    std::vector<ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j = 0; j < numArgs; ++j)
      arguments.push_back(eval(ki, j + 1, state).value);

    if (f) {
      const FunctionType *fType = dyn_cast<FunctionType>(
          cast<PointerType>(f->getType())->getElementType());
      const FunctionType *fpType = dyn_cast<FunctionType>(
          cast<PointerType>(fp->getType())->getElementType());

      // special case the call with a bitcast case
      if (fType != fpType) {
        assert(fType && fpType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i = 0;
        for (std::vector<ref<Expr> >::iterator ai = arguments.begin(),
                                               ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();

          if (i < fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
// XXX need to check other param attrs ?
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
              bool isSExt = cs.paramHasAttr(i + 1, llvm::Attribute::SExt);
#elif LLVM_VERSION_CODE >= LLVM_VERSION(3, 2)
              bool isSExt = cs.paramHasAttr(i + 1, llvm::Attributes::SExt);
#else
            bool isSExt = cs.paramHasAttr(i + 1, llvm::Attribute::SExt);
#endif
              if (isSExt) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }

          i++;
        }
      }
      executeCall(state, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, state).value;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void)success;
        StatePair res = fork(*free, EqExpr::create(v, value), true);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function *)addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once((void *)(unsigned long)addr,
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
    ref<Expr> result = eval(ki, state.incomingBBIndex, state).value;
#else
    ref<Expr> result = eval(ki, state.incomingBBIndex * 2, state).value;
#endif
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
      txTree->executePHI(i, state.incomingBBIndex, result);
#else
      txTree->executePHI(i, state.incomingBBIndex * 2, result);
#endif
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:executePHI] PHI, Node:" << state.txTreeNode->getNodeSequenceNumber()
                     << " : " << state.incomingBBIndex << "\n";
      if (txTree->getPhiValuesFlag()) {
        txTree->setPhiValue(i, result);
        if (DebugTracerX)
          llvm::errs() << "[executeInstruction:setPhiValue] PHI, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }
    }

    break;
  }

  // Special instructions
  case Instruction::Select: {
    ref<Expr> cond = eval(ki, 0, state).value;
    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, tExpr, fExpr);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] Select, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

  // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AddExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] Add, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " + Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SubExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] Sub, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " - Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = MulExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] Mul, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " * Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = UDivExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] UDiv, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " / Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SDivExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] SDiv, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " / Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = URemExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] URem, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " % Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SRemExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] SRem, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " % Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] And, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " And Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] Or, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << "Or Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX) {
        llvm::errs() << "[executeInstruction:execute] Xor, Node:" << state.txTreeNode->getNodeSequenceNumber() << ", Left:";
        left->print(llvm::errs());
        llvm::errs() << " Xor Right:";
        left->print(llvm::errs());
        llvm::errs() << "\n";
      }
    }
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ShlExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] Shl, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = LShrExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
    if (DebugTracerX)
      llvm::errs() << "[executeInstruction:execute] LShr, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AShrExpr::create(left, right);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] AShr, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);
    ref<Expr> result, left, right;

    switch (ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = EqExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = NeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = UgtExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = UgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = UltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = UleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = SgtExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = SgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = SltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      left = eval(ki, 0, state).value;
      right = eval(ki, 1, state).value;
      result = SleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] ICMP, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  // Memory instructions...
  case Instruction::Alloca: {
    AllocaInst *ai = cast<AllocaInst>(i);
    unsigned elementSize =
        kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, state).value;
      count = Expr::createZExtToPointerWidth(count);
      size = MulExpr::create(size, count);
    }
    executeAlloc(state, size, true, ki);
    break;
  }

  case Instruction::Load: {
    ref<Expr> base = eval(ki, 0, state).value;
    executeMemoryOperation(state, false, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    executeMemoryOperation(state, true, base, value, ki);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction *>(ki);
    ref<Expr> base = eval(ki, 0, state).value;
    ref<Expr> address(base);
    ref<Expr> offset(Expr::createPointer(0));

    for (std::vector<std::pair<unsigned, uint64_t> >::iterator
             it = kgepi->indices.begin(),
             ie = kgepi->indices.end();
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, state).value;
      address = AddExpr::create(
          address, MulExpr::create(Expr::createSExtToPointerWidth(index),
                                   Expr::createPointer(elementSize)));
      if (INTERPOLATION_ENABLED) {
        offset = AddExpr::create(
            offset, MulExpr::create(Expr::createSExtToPointerWidth(index),
                                    Expr::createPointer(elementSize)));
      }
    }
    if (kgepi->offset) {
      address = AddExpr::create(address, Expr::createPointer(kgepi->offset));
      if (INTERPOLATION_ENABLED) {
        offset = AddExpr::create(offset, Expr::createPointer(kgepi->offset));
      }
    }
    bindLocal(ki, state, address);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, address, base, offset);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] GetElementPtr, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> arg = eval(ki, 0, state).value;
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value, 0,
                                           getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, arg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] Trunc, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> arg = eval(ki, 0, state).value;
    ref<Expr> result =
        ZExtExpr::create(arg, getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, arg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] ZExt, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> arg = eval(ki, 0, state).value;
    ref<Expr> result =
        SExtExpr::create(arg, getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, arg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] SExt, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    ref<Expr> result = ZExtExpr::create(arg, pType);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, arg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] IntToPtr, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    ref<Expr> result = ZExtExpr::create(arg, iType);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, arg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] PtrToInt, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] BitCast, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()),
                      left->getAPValue());
    Res.add(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()),
        APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.add(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    ref<Expr> result = ConstantExpr::alloc(Res.bitcastToAPInt());
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FAdd, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()),
                      left->getAPValue());
    Res.subtract(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()),
        APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.subtract(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    ref<Expr> result = ConstantExpr::alloc(Res.bitcastToAPInt());
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FSub, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FMul: {
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()),
                      left->getAPValue());
    Res.multiply(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()),
        APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.multiply(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    ref<Expr> result = ConstantExpr::alloc(Res.bitcastToAPInt());
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FMul, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()),
                      left->getAPValue());
    Res.divide(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()),
        APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.divide(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    ref<Expr> result = ConstantExpr::alloc(Res.bitcastToAPInt());
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FDiv, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(left->getWidth()),
                      left->getAPValue());
    Res.mod(
        APFloat(*fpWidthToSemantics(right->getWidth()), right->getAPValue()),
        APFloat::rmNearestTiesToEven);
#else
    llvm::APFloat Res(left->getAPValue());
    Res.mod(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
#endif
    ref<Expr> result = ConstantExpr::alloc(Res.bitcastToAPInt());
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FRem, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Res(arg->getAPValue());
#endif
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven, &losesInfo);
    ref<Expr> result = ConstantExpr::alloc(Res);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FPTrunc, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Res(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Res(arg->getAPValue());
#endif
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven, &losesInfo);
    ref<Expr> result = ConstantExpr::alloc(Res);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FPExt, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Arg(arg->getAPValue());
#endif
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false, llvm::APFloat::rmTowardZero,
                         &isExact);
    ref<Expr> result = ConstantExpr::alloc(value, resultType);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FPToUI, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    llvm::APFloat Arg(*fpWidthToSemantics(arg->getWidth()), arg->getAPValue());
#else
    llvm::APFloat Arg(arg->getAPValue());

#endif
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, true, llvm::APFloat::rmTowardZero,
                         &isExact);
    ref<Expr> result = ConstantExpr::alloc(value, resultType);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FPToSI, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    ref<Expr> result = ConstantExpr::alloc(f);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] UIToFP, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<Expr> origArg = eval(ki, 0, state).value;
    ref<ConstantExpr> arg = toConstant(state, origArg, "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    ref<Expr> result = ConstantExpr::alloc(f);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, origArg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] SIToFP, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left =
        toConstant(state, eval(ki, 0, state).value, "floating point");
    ref<ConstantExpr> right =
        toConstant(state, eval(ki, 1, state).value, "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
    APFloat LHS(*fpWidthToSemantics(left->getWidth()), left->getAPValue());
    APFloat RHS(*fpWidthToSemantics(right->getWidth()), right->getAPValue());
#else
    APFloat LHS(left->getAPValue());
    APFloat RHS(right->getAPValue());
#endif
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch (fi->getPredicate()) {
    // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = CmpRes != APFloat::cmpUnordered;
      break;

    case FCmpInst::FCMP_UNO:
      Result = CmpRes == APFloat::cmpUnordered;
      break;

    // Ordered comparisons return false if either operand is NaN.  Unordered
    // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OEQ:
      Result = CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UGT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGT:
      Result = CmpRes == APFloat::cmpGreaterThan;
      break;

    case FCmpInst::FCMP_UGE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGE:
      Result = CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_ULT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLT:
      Result = CmpRes == APFloat::cmpLessThan;
      break;

    case FCmpInst::FCMP_ULE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLE:
      Result = CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UNE:
      Result = CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual;
      break;
    case FCmpInst::FCMP_ONE:
      Result = CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual;
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    ref<Expr> result = ConstantExpr::alloc(Result, Expr::Bool);
    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, left, right);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] FCmp, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction *>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset * 8,
             rOffset = kgepi->offset * 8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, agg, val);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] InsertValue, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction *>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset * 8,
                                           getWidthForLLVMType(i->getType()));

    bindLocal(ki, state, result);

    // Update dependency
    if (INTERPOLATION_ENABLED) {
      txTree->execute(i, result, agg);
      if (DebugTracerX)
        llvm::errs() << "[executeInstruction:execute] ExtractValue, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    break;
  }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 3)
  case Instruction::Fence: {
    // Ignore for now
    break;
  }
#endif

  // Other instructions...
  // Unhandled
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    terminateStateOnError(state, "XXX vector instructions unhandled",
                          Unhandled);
    break;

  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    searcher->update(current, addedStates, removedStates);
  }

  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();

  for (std::vector<ExecutionState *>::iterator it = removedStates.begin(),
                                               ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState *>::iterator it2 = states.find(es);
    assert(it2 != states.end());
    states.erase(it2);
    std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it3 =
        seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    processTree->remove(es->ptreeNode);
    if (INTERPOLATION_ENABLED) {
      txTree->remove(es, solver, (current == 0));
      if (DebugTracerX)
        llvm::errs() << "[updateStates:remove] Node:" << es->txTreeNode->getNodeSequenceNumber() << "\n";
    }
    delete es;
  }
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
      ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (LLVM_TYPE_Q StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned)ci->getZExtValue());
      constantOffset = constantOffset->Add(
          ConstantExpr::alloc(addend, Context::get().getPointerWidth()));
    } else {
      const SequentialType *set = cast<SequentialType>(*ii);
      uint64_t elementSize =
          kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index =
            evalConstant(c)->SExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = index->Mul(
            ConstantExpr::alloc(elementSize, Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
    }
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction *>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (std::vector<KFunction *>::iterator it = kmodule->functions.begin(),
                                          ie = kmodule->functions.end();
       it != ie; ++it) {
    KFunction *kf = *it;
    for (unsigned i = 0; i < kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable = new Cell[kmodule->constants.size()];
  for (unsigned i = 0; i < kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule->constants[i]);
  }
}

void Executor::checkMemoryUsage() {
  if (!MaxMemory)
    return;
  if ((stats::instructions & 0xFFFF) == 0) {
    // We need to avoid calling GetTotalMallocUsage() often because it
    // is O(elts on freelist). This is really bad since we start
    // to pummel the freelist once we hit the memory cap.
    unsigned mbs = (util::GetTotalMallocUsage() >> 20) +
                   (memory->getUsedDeterministicSize() >> 20);

    if (mbs > MaxMemory) {
      if (mbs > MaxMemory + 100) {
        // just guess at how many to kill
        unsigned numStates = states.size();
        unsigned toKill = std::max(1U, numStates - numStates * MaxMemory / mbs);
        klee_warning("killing %d states (over memory cap)", toKill);
        std::vector<ExecutionState *> arr(states.begin(), states.end());
        for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
          unsigned idx = rand() % N;
          // Make two pulls to try and not hit a state that
          // covered new code.
          if (arr[idx]->coveredNew)
            idx = rand() % N;

          std::swap(arr[idx], arr[N - 1]);
          terminateStateEarly(*arr[N - 1], "Memory limit exceeded.");
        }
      }
      atMemoryLimit = true;
    } else {
      atMemoryLimit = false;
    }
  }
}

void Executor::doDumpStates() {
  if (!DumpStatesOnHalt || states.empty())
    return;
  klee_message("halting execution, dumping remaining states");
  for (std::set<ExecutionState *>::iterator it = states.begin(),
                                            ie = states.end();
       it != ie; ++it) {
    ExecutionState &state = **it;
    stepInstruction(state); // keep stats rolling
    terminateStateEarly(state, "Execution halting.");
  }
  updateStates(0);
}

std::map<int, std::set<std::string> >
Executor::readBBOrderToSpecAvoid(std::string folderName) {
  std::map<int, std::set<std::string> > res;
  DIR *dirp = opendir(folderName.c_str());
  dirent *dp;
  while ((dp = readdir(dirp)) != NULL) {
    std::string name(dp->d_name);
    if (strcmp(name.substr(0, 10).c_str(), "SpecAvoid_") == 0) {
      std::string absPath = folderName + "/" + name;
      std::pair<int, std::set<std::string> > tmp = readBBSpecAvoid(absPath);
      res[tmp.first] = tmp.second;
    }
  }
  (void)closedir(dirp);
  return res;
}

std::pair<int, std::set<std::string> >
Executor::readBBSpecAvoid(std::string fileName) {
  bool isFirst = true;
  int bb;
  std::set<std::string> avoid;
  std::ifstream in(fileName.c_str());
  std::string str;
  while (std::getline(in, str)) {
    if (isFirst) {
      bb = atoi(str.c_str());
      isFirst = false;
    } else {
      if (!TxSpeculationHelper::trim(str).empty())
        avoid.insert(TxSpeculationHelper::trim(str));
    }
  }
  in.close();
  return std::make_pair(bb, avoid);
}
std::set<llvm::BasicBlock *> Executor::readVisitedBB(std::string fileName) {
  std::set<int> bbs;
  std::ifstream in(fileName.c_str());
  std::string str;
  while (std::getline(in, str)) {
    if (!TxSpeculationHelper::trim(str).empty()) {
      int bb = atoi(str.c_str());
      bbs.insert(bb);
    }
  }
  in.close();
  std::set<llvm::BasicBlock *> res;
  for (std::map<llvm::Function *, std::map<llvm::BasicBlock *, int> >::iterator
           it = fBBOrder.begin(),
           ie = fBBOrder.end();
       it != ie; ++it) {
    for (std::map<llvm::BasicBlock *, int>::iterator it1 = it->second.begin(),
                                                     ie1 = it->second.end();
         it1 != ie1; ++it1) {
      if (bbs.find(it1->second) != bbs.end()) {
        res.insert(it1->first);
      }
    }
  }
  return res;
}

void Executor::run(ExecutionState &initialState) {
  if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC) {
    independenceYes = 0;
    independenceNo = 0;
    dynamicYes = 0;
    dynamicNo = 0;
    specFail = 0;
    totalSpecFailTime = 0.0;
    for (std::map<llvm::Instruction *, unsigned int>::iterator
             it = specSnap.begin(),
             ie = specSnap.end();
         it != ie; ++it) {
      it->second = 0;
    }
    // load avoid BB
    bbOrderToSpecAvoid = readBBOrderToSpecAvoid(DependencyFolder);
    visitedBlocks = readVisitedBB(DependencyFolder + "/InitialVisitedBB.txt");
  }

  startingBBPlottingTime = time(0);
  // get interested source code
  size_t lastindex = InputFile.find_last_of(".");
  std::string InputFile1 = InputFile.substr(0, lastindex);
  lastindex = InputFile1.find_last_of("/");
  std::string InputFile2 = InputFile1.substr(lastindex + 1);
  covInterestedSourceFileName = InputFile2 + ".c";

  // BB to order
  allBlockCount = 0;
  for (std::map<llvm::Function *, KFunction *>::iterator
           it = kmodule->functionMap.begin(),
           ie = kmodule->functionMap.end();
       it != ie; ++it) {
    Function *f = it->first;
    // get source file of the funtion
    KFunction *kf = it->second;
    KInstruction *ki = kf->instructions[0];
    const std::string path = ki->info->file;
    std::size_t botDirPos = path.find_last_of("/");
    std::string sourceFileName = path.substr(botDirPos + 1, path.length());
    // if the source file is interested then loop over its BBs
    if ((sourceFileName == covInterestedSourceFileName) &&
        isCoverableFunction(f)) {
      // loop over BBs of function
      std::vector<llvm::BasicBlock *> bbs;
      for (llvm::Function::iterator b = f->begin(); b != f->end(); ++b) {
        fBBOrder[f][b] = ++allBlockCount;
        if (BBCoverage >= 4) {
          // Print All atomic condition covered
          std::string liveBBFileAICMP =
              interpreterHandler->getOutputFilename("coveredAICMP.txt");
          std::ofstream liveBBFileAICMPOut(liveBBFileAICMP.c_str(),
                                           std::ofstream::app);

          // block content
          std::string tmpICMP;
          raw_string_ostream tmpICMPOS(tmpICMP);
          for (llvm::BasicBlock::iterator aicmp = b->begin(); aicmp != b->end();
               aicmp++) {
            if (llvm::isa<llvm::ICmpInst>(aicmp)) {
              allICMPCount++;
              liveBBFileAICMPOut << "Function: "
                                 << b->getParent()->getName().str() << " ";
              liveBBFileAICMPOut << "Block Order: " << allBlockCount;
              aicmp->print(tmpICMPOS);
              liveBBFileAICMPOut << tmpICMP << "\n";
            }
          }
          liveBBFileAICMPOut.close();
        }
      }
    }
  }

  // first BB of main()
  KInstruction *ki = initialState.pc;
  BasicBlock *firstBB = ki->inst->getParent();
  if (fBBOrder.find(firstBB->getParent()) != fBBOrder.end() &&
      fBBOrder[firstBB->getParent()].find(firstBB) !=
          fBBOrder[firstBB->getParent()].end()) {
    processBBCoverage(BBCoverage, ki->inst->getParent(), false);
  }
  bindModuleConstants();

  // Delay init till now so that ticks don't accrue during
  // optimization and such.
  initTimers();

  states.insert(&initialState);

  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    for (std::vector<KTest *>::const_iterator it = usingSeeds->begin(),
                                              ie = usingSeeds->end();
         it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size() + 10;
    double lastTime, startTime = lastTime = util::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) {
        doDumpStates();
        return;
      }

      std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
          seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      unsigned numSeeds = it->second.size();
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc;

      if (INTERPOLATION_ENABLED) {
        // We synchronize the node id to that of the state. The node id is set
        // only when it was the address of the first instruction in the node.
        txTree->setCurrentINode(state);
        if (DebugTracerX)
          llvm::errs() << "[run:setCurrentINode] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }

      stepInstruction(state);

      executeInstruction(state, ki);
      processTimers(&state, MaxInstructionTime * numSeeds);
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState *, std::vector<SeedInfo> >::iterator
                 it = seedMap.begin(),
                 ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        double time = util::getWallTime();
        if (SeedTime > 0. && time > startTime + SeedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds <= lastNumSeeds - 10 || time >= lastTime + 10) {
          lastTime = time;
          lastNumSeeds = numSeeds;
          klee_message("%d seeds remaining over: %d states", numSeeds,
                       numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int)states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState *>::iterator it = states.begin(),
                                              ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed) {
      doDumpStates();
      return;
    }
  }

  searcher = constructUserSearcher(*this);

  std::vector<ExecutionState *> newStates(states.begin(), states.end());
  searcher->update(0, newStates, std::vector<ExecutionState *>());

  while (!states.empty() && !haltExecution) {
    ExecutionState &state = searcher->selectState();

#ifdef ENABLE_Z3
    if (INTERPOLATION_ENABLED) {
      // We synchronize the node id to that of the state. The node id
      // is set only when it was the address of the first instruction
      // in the node.
      txTree->setCurrentINode(state);
      if (DebugTracerX)
        llvm::errs() << "[run:setCurrentINode] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";


      uint64_t debugLevel = txTree->getDebugState();

      if (debugLevel > 0) {
        std::string debugMessage;
        llvm::raw_string_ostream stream(debugMessage);
        if (debugLevel > 1) {
          stream << "\nCurrent state:\n";
          processTree->print(stream);
          stream << "\n";
          txTree->print(stream);
          stream << "\n";
          stream << "--------------------------- Current Node "
                    "----------------------------\n";
          state.txTreeNode->print(stream);
          stream << "\n";
        }
        stream << "------------------- Executing New Instruction "
                  "-----------------------\n";
        if (outputFunctionName(state.pc->inst, stream))
          stream << ":";
        state.pc->inst->print(stream);
        stream << "\n";
        stream.flush();

        klee_message("%s", debugMessage.c_str());
      }
    }
#endif

    if (INTERPOLATION_ENABLED &&
        txTree->subsumptionCheck(solver, state, coreSolverTimeout)) {
      terminateStateOnSubsumption(state);
      if (DebugTracerX)
        llvm::errs() << "[run:subsumptionCheck] Pass, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    } else {
      KInstruction *ki = state.pc;
      stepInstruction(state);

      executeInstruction(state, ki);
      if (INTERPOLATION_ENABLED) {
        state.txTreeNode->incInstructionsDepth();
        if (DebugTracerX)
          llvm::errs() << "[run:subsumptionCheck] Fail, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
      }
      processTimers(&state, MaxInstructionTime);

      checkMemoryUsage();
    }
    updateStates(&state);
  }

  delete searcher;
  searcher = 0;

  doDumpStates();
}

std::string Executor::getAddressInfo(ExecutionState &state,
                                     ref<Expr> address) const {
  std::string Str;
  llvm::raw_string_ostream info(Str);
  info << "\taddress: " << address << "\n";
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void)success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair<ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second << "]\n";
  }

  MemoryObject hack((unsigned)example);
  MemoryMap::iterator lower = state.addressSpace.objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower == state.addressSpace.objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower != state.addressSpace.objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower == state.addressSpace.objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}

void Executor::terminateState(ExecutionState &state) {
  if (replayKTest && replayPosition != replayKTest->numObjects) {
    klee_warning_once(replayKTest,
                      "replay did not consume all objects in test input.");
  }

  interpreterHandler->incPathsExplored();

  std::vector<ExecutionState *>::iterator it =
      std::find(addedStates.begin(), addedStates.end(), &state);
  if (it == addedStates.end()) {
    state.pc = state.prevPC;

    removedStates.push_back(&state);
  } else {
    // never reached searcher, just delete immediately
    std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it3 =
        seedMap.find(&state);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    addedStates.erase(it);
    processTree->remove(state.ptreeNode);

    if (INTERPOLATION_ENABLED) {
      txTree->remove(&state, solver, false);
      if (DebugTracerX)
        llvm::errs() << "[terminateState:remove] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    delete &state;
  }
}

void Executor::terminateStateOnSubsumption(ExecutionState &state) {
  assert(INTERPOLATION_ENABLED);

  // Implementationwise, basically the same as terminateStateEarly method,
  // but with different statistics functions called, and empty error
  // message as this is not an error.
  interpreterHandler->incSubsumptionTermination();
  interpreterHandler->incInstructionsDepthOnSubsumption(state.depth);
  interpreterHandler->incTotalInstructionsOnSubsumption(
      state.txTreeNode->getInstructionsDepth());

#ifdef ENABLE_Z3
  if (SubsumedTest && (!OnlyOutputStatesCoveringNew || state.coveredNew ||
                       (AlwaysOutputSeeds && seedMap.count(&state)))) {
    interpreterHandler->incSubsumptionTerminationTest();
    interpreterHandler->processTestCase(state, 0, "early");
  }
#endif
  terminateState(state);
}

void Executor::terminateStateEarly(ExecutionState &state,
                                   const Twine &message) {
  interpreterHandler->incEarlyTermination();
  if (INTERPOLATION_ENABLED) {
    interpreterHandler->incBranchingDepthOnEarlyTermination(state.depth);
    interpreterHandler->incInstructionsDepthOnEarlyTermination(
        state.txTreeNode->getInstructionsDepth());
    state.txTreeNode->setGenericEarlyTermination();
  }

  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state))) {
    interpreterHandler->incEarlyTerminationTest();
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
  }
  terminateState(state);
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  interpreterHandler->incExitTermination();
  if (INTERPOLATION_ENABLED) {
    interpreterHandler->incBranchingDepthOnExitTermination(state.depth);
    interpreterHandler->incTotalInstructionsOnExit(
        state.txTreeNode->getInstructionsDepth());
  }

  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state))) {
    interpreterHandler->incExitTerminationTest();
    interpreterHandler->processTestCase(state, 0, 0);
  }
  terminateState(state);
}

const InstructionInfo &
Executor::getLastNonKleeInternalInstruction(const ExecutionState &state,
                                            Instruction **lastInstruction) {
  // unroll the stack of the applications state and find
  // the last instruction which is not inside a KLEE internal function
  ExecutionState::stack_ty::const_reverse_iterator it = state.stack.rbegin(),
                                                   itE = state.stack.rend();

  // don't check beyond the outermost function (i.e. main())
  itE--;

  const InstructionInfo *ii = 0;
  if (kmodule->internalFunctions.count(it->kf->function) == 0) {
    ii = state.prevPC->info;
    *lastInstruction = state.prevPC->inst;
    //  Cannot return yet because even though
    //  it->function is not an internal function it might of
    //  been called from an internal function.
  }

  // Wind up the stack and check if we are in a KLEE internal function.
  // We visit the entire stack because we want to return a CallInstruction
  // that was not reached via any KLEE internal functions.
  for (; it != itE; ++it) {
    // check calling instruction and if it is contained in a KLEE internal
    // function
    const Function *f = (*it->caller).inst->getParent()->getParent();
    if (kmodule->internalFunctions.count(f)) {
      ii = 0;
      continue;
    }
    if (!ii) {
      ii = (*it->caller).info;
      *lastInstruction = (*it->caller).inst;
    }
  }

  if (!ii) {
    // something went wrong, play safe and return the current instruction info
    *lastInstruction = state.prevPC->inst;
    return *state.prevPC->info;
  }
  return *ii;
}

bool Executor::shouldExitOn(enum TerminateReason termReason) {
  std::vector<TerminateReason>::iterator s = ExitOnErrorType.begin();
  std::vector<TerminateReason>::iterator e = ExitOnErrorType.end();

  for (; s != e; ++s)
    if (termReason == *s)
      return true;

  return false;
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     enum TerminateReason termReason,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set<std::pair<Instruction *, std::string> > emittedErrors;
  Instruction *lastInst;
  const InstructionInfo &ii =
      getLastNonKleeInternalInstruction(state, &lastInst);

  if (INTERPOLATION_ENABLED && SpecTypeToUse != NO_SPEC &&
      SpecStrategyToUse != TIMID && state.txTreeNode->isSpeculationNode()) {
    //    llvm::outs() << "=== start jumpback because of error \n";
    specFail++;
    speculativeBackJump(state);
    klee_message("Speculation Failed: %s:%d: %s", ii.file.c_str(), ii.line,
                 message.c_str());
    return;
  }

  interpreterHandler->incErrorTermination();
  if (INTERPOLATION_ENABLED) {
    interpreterHandler->incBranchingDepthOnErrorTermination(state.depth);
    interpreterHandler->incInstructionsDepthOnErrorTermination(
        state.txTreeNode->getInstructionsDepth());

    if (termReason == Executor::Assert) {
      TxTreeGraph::setError(state, TxTreeGraph::ASSERTION);
      if (DebugTracerX)
        llvm::errs() << "[terminateStateOnError:setError] ASSERTION, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    } else if (termReason == Executor::Ptr &&
               messaget.str() == "memory error: out of bound pointer") {
      TxTreeGraph::setError(state, TxTreeGraph::MEMORY);
      if (DebugTracerX)
        llvm::errs() << "[terminateStateOnError:setError] MEMORY, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    } else {
      state.txTreeNode->setGenericEarlyTermination();
      TxTreeGraph::setError(state, TxTreeGraph::GENERIC);
      if (DebugTracerX)
        llvm::errs() << "[terminateStateOnError:setError] GENERIC, Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }
    if (WPInterpolant)
      state.txTreeNode->setAssertionFail(EmitAllErrors);
  }

  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(lastInst, message)).second) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line,
                   message.c_str());
    } else {
      klee_message("ERROR: (location information missing) %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");

    std::string MsgString;
    llvm::raw_string_ostream msg(MsgString);
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
      msg << "assembly.ll line: " << ii.assemblyLine << "\n";
    }
    msg << "Stack: \n";
    state.dumpStack(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;

    std::string suffix_buf;
    if (!suffix) {
      suffix_buf = TerminateReasonNames[termReason];
      suffix_buf += ".err";
      suffix = suffix_buf.c_str();
    }

    interpreterHandler->incErrorTerminationTest();
    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
  }
  if (!EmitAllErrorsInSamePath) {
    terminateState(state);
  }

  if (shouldExitOn(termReason))
    haltExecution = true;
}

// XXX shoot me
static const char *okExternalsList[] = {
  "printf", "fprintf", "puts", "getpid"
};
static std::set<std::string>
okExternals(okExternalsList, okExternalsList + (sizeof(okExternalsList) /
                                                sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state, KInstruction *target,
                                    Function *function,
                                    std::vector<ref<Expr> > &arguments) {
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;

  if (NoExternals && !okExternals.count(function->getName())) {
    klee_warning("Calling not-OK external function : %s\n",
                 function->getName().str().c_str());
    terminateStateOnError(state, "externals disallowed", User);
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args =
      (uint64_t *)alloca(2 * sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(),
                                         ae = arguments.end();
       ai != ae; ++ai) {
    if (AllowExternalSymCalls) { // don't bother checking uniqueness
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      ce->toMemory(&args[wordIndex]);
      wordIndex += (ce->getWidth() + 63) / 64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth() + 63) / 64;
      } else {
        terminateStateOnExecError(state,
                                  "external call with symbolic argument: " +
                                      function->getName());
        return;
      }
    }
  }

  state.addressSpace.copyOutConcretes();

  if (!SuppressExternalWarnings) {

    std::string TmpStr;
    llvm::raw_string_ostream os(TmpStr);
    os << "calling external: " << function->getName().str() << "(";
    for (unsigned i = 0; i < arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size() - 1)
        os << ", ";
    }
    os << ")";

    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }

  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          External);
    return;
  }

  if (!state.addressSpace.copyInConcretes()) {
    terminateStateOnError(state, "external modified read-only object",
                          External);
    return;
  }

  LLVM_TYPE_Q Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(getGlobalContext())) {
    ref<Expr> e =
        ConstantExpr::fromMemory((void *)args, getWidthForLLVMType(resultType));
    bindLocal(target, state, e);

    if (INTERPOLATION_ENABLED) {
      std::vector<ref<Expr> > tmpArgs;
      tmpArgs.push_back(e);
      for (unsigned i = 0; i < arguments.size(); ++i) {
        tmpArgs.push_back(arguments.at(i));
      }
      txTree->execute(target->inst, tmpArgs);
      if (DebugTracerX)
        llvm::errs() << "[callExternalFunction:execute] Node:" << state.txTreeNode->getNodeSequenceNumber()
                     << ", Inst:" << target->inst->getOpcodeName() << "\n";
    }
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state,
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayKTest || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason to?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() % n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.

  static unsigned id;
  const std::string arrayName("rrws_arr" + llvm::utostr(++id));
  const unsigned arrayWidth(Expr::getMinBytesForWidth(e->getWidth()));
  const Array *array = arrayCache.CreateArray(arrayName, arrayWidth);
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  llvm::errs() << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);

  if (INTERPOLATION_ENABLED) {
    // We create shadow array as existentially-quantified
    // variables for subsumption checking
    const Array *shadow = arrayCache.CreateArray(
        TxShadowArray::getShadowName(arrayName), arrayWidth);
    TxShadowArray::addShadowArrayMap(array, shadow);
    if (DebugTracerX)
      llvm::errs() << "[replaceReadWithSymbolic:addShadowArrayMap] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
  }

  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state,
                                         const MemoryObject *mo, bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace.bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack.back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state, ref<Expr> size, bool isLocal,
                            KInstruction *target, bool zeroMemory,
                            const ObjectState *reallocFrom) {
  size = toUnique(state, size);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    MemoryObject *mo = memory->allocate(CE->getZExtValue(), isLocal, false,
                                        state.prevPC->inst);
    if (!mo) {
      bindLocal(target, state,
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      ObjectState *os = bindObjectInState(state, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
      bindLocal(target, state, mo->getBaseExpr());

      // Update dependency
      if (INTERPOLATION_ENABLED) {
        txTree->execute(target->inst, mo->getBaseExpr(), size);
        if (DebugTracerX)
          llvm::errs() << "[executeAlloc:execute] Node:" << state.txTreeNode->getNodeSequenceNumber()
                       << ", Inst:" << target->inst->getOpcodeName() << "\n";
      }

      if (reallocFrom) {
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i = 0; i < count; i++)
          os->write(i, reallocFrom->read8(i));
        state.addressSpace.unbindObject(reallocFrom->getObject());
      }
    }
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    //
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void)success;

    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (!res)
        break;
      example = tmp;
    }

    StatePair fixedSize = fork(state, EqExpr::create(example, size), true);

    if (fixedSize.second) {
      // Check for exactly two values
      ref<ConstantExpr> tmp;
      bool success = solver->getValue(*fixedSize.second, size, tmp);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      bool res;
      success =
          solver->mustBeTrue(*fixedSize.second, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void)success;
      if (res) {
        executeAlloc(*fixedSize.second, tmp, isLocal, target, zeroMemory,
                     reallocFrom);
      } else {
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize =
            fork(*fixedSize.second,
                 UltExpr::create(ConstantExpr::alloc(1 << 31, W), size), true);
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returning 0");
          ref<Expr> result =
              ConstantExpr::alloc(0, Context::get().getPointerWidth());
          bindLocal(target, *hugeSize.first, result);

          // Update dependency
          if (INTERPOLATION_ENABLED) {
            txTree->execute(target->inst, result);
            if (DebugTracerX)
              llvm::errs() << "[executeAlloc:execute] symbolic, Node:" << state.txTreeNode->getNodeSequenceNumber()
                           << ", Inst:" << target->inst->getOpcodeName()  << "\n";
          }
        }

        if (hugeSize.second) {

          std::string Str;
          llvm::raw_string_ostream info(Str);
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << tmp << "\n";
          terminateStateOnError(*hugeSize.second, "concretized symbolic size",
                                Model, NULL, info.str());
        }
      }
    }

    if (fixedSize.first) // can be zero when fork fails
      executeAlloc(*fixedSize.first, example, isLocal, target, zeroMemory,
                   reallocFrom);
  }
}

void Executor::executeFree(ExecutionState &state, ref<Expr> address,
                           KInstruction *target) {
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "free");

    for (Executor::ExactResolutionList::iterator it = rl.begin(), ie = rl.end();
         it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, "free of alloca", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, "free of global", Free, NULL,
                              getAddressInfo(*it->second, address));
      } else {
        it->second->addressSpace.unbindObject(mo);
        if (target)
          bindLocal(target, *it->second, Expr::createPointer(0));
      }
    }
  }
}

void Executor::resolveExact(ExecutionState &state, ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name) {
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace.resolve(state, solver, p, rl);

  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); it != ie;
       ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());

    StatePair branches = fork(*unbound, inBounds, true);

    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    terminateStateOnError(*unbound, "memory error: invalid pointer: " + name,
                          Ptr, NULL, getAddressInfo(*unbound, p));
  }
}

void Executor::executeMemoryOperation(ExecutionState &state, bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target) {
  Expr::Width type = (isWrite ? value->getWidth()
                              : getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints.simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints.simplifyExpr(value);
  }

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(coreSolverTimeout);
  if (!state.addressSpace.resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace.resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    const MemoryObject *mo = op.first;

    if (MaxSymArraySize && mo->size >= MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }

    ref<Expr> offset = mo->getOffsetExpr(address);
    ref<Expr> boundsCheck = mo->getBoundsCheckOffset(offset, bytes);

    bool inBounds;
    solver->setTimeout(coreSolverTimeout);
    bool success = solver->mustBeTrue(state, boundsCheck, inBounds);
    solver->setTimeout(0);
    if (!success) {
      state.pc = state.prevPC;
      terminateStateEarly(state, "Query timed out (bounds check).");
      return;
    }

    if (inBounds) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = state.addressSpace.getWriteable(mo, os);
          wos->write(offset, value);

          // Update dependency
          if (INTERPOLATION_ENABLED && target) {
            if (DebugTracerX)
              llvm::errs() << "[executeMemoryOperation:executeMemoryOperation] isWrite, Node:" << state.txTreeNode->getNodeSequenceNumber()
                           << ", Inst:" << target->inst->getOpcodeName() << "\n";
          }
          if (INTERPOLATION_ENABLED && target &&
              txTree->executeMemoryOperation(target->inst, value, address,
                                             inBounds)) {
            // Memory error according to Tracer-X
            terminateStateOnError(state, "memory error: out of bound pointer",
                                  Ptr, NULL, getAddressInfo(state, address));
          }
        }
      } else {
        ref<Expr> result = os->read(offset, type);

        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);

        bindLocal(target, state, result);

        // Update dependency
        if (INTERPOLATION_ENABLED && target) {
          if (DebugTracerX)
            llvm::errs() << "[executeMemoryOperation:executeMemoryOperation] !isWrite, Node:" << state.txTreeNode->getNodeSequenceNumber()
                         << ", Inst:" << target->inst->getOpcodeName() << "\n";
        }
        if (INTERPOLATION_ENABLED && target &&
            txTree->executeMemoryOperation(target->inst, result, address,
                                           inBounds)) {
          // Memory error according to Tracer-X
          terminateStateOnError(state, "memory error: out of bound pointer",
                                Ptr, NULL, getAddressInfo(state, address));
        }
      }

      return;
    }
  }

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)

  ResolutionList rl;
  solver->setTimeout(coreSolverTimeout);
  bool incomplete = state.addressSpace.resolve(state, solver, address, rl, 0,
                                               coreSolverTimeout);
  solver->setTimeout(0);

  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;

  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);

    StatePair branches = fork(*unbound, inBounds, true);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped
    if (bound) {
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound, "memory error: object read only",
                                ReadOnly);
        } else {
          ObjectState *wos = bound->addressSpace.getWriteable(mo, os);
          wos->write(mo->getOffsetExpr(address), value);

          // Update dependency
          if (INTERPOLATION_ENABLED && target) {
            TxTree::executeOnNode(bound->txTreeNode, target->inst, value,
                                  address);
            if (DebugTracerX)
              llvm::errs() << "[executeMemoryOperation:executeOnNode] Node:" << state.txTreeNode->getNodeSequenceNumber()
                           << ", Inst:" << target->inst->getOpcodeName() << "\n";

          }
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
        bindLocal(target, *bound, result);

        // Update dependency
        if (INTERPOLATION_ENABLED && target) {
          TxTree::executeOnNode(bound->txTreeNode, target->inst, result,
                                address);
          if (DebugTracerX)
            llvm::errs() << "[executeMemoryOperation:executeOnNode] Node:" << state.txTreeNode->getNodeSequenceNumber()
                         << ", Inst:" << target->inst->getOpcodeName() << "\n";
        }
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }

  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (INTERPOLATION_ENABLED) {
      TxTree::symbolicExecutionError =
          true; // We let interpolation subsystem knows we are recovering from
                // error, hence the previous expression may not be recorded
      if (DebugTracerX)
        llvm::errs() << "[executeMemoryOperation:symbolicExecutionError] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }

    if (incomplete) {
      terminateStateEarly(*unbound, "Query timed out (resolve).");
    } else {
      if (INTERPOLATION_ENABLED && target) {
        state.txTreeNode->memoryBoundViolationInterpolation(target->inst,
                                                            address);
        if (DebugTracerX)
          llvm::errs() << "[executeMemoryOperation:memoryBoundViolationInterpolation] Node:" << state.txTreeNode->getNodeSequenceNumber()
                       << ", Inst:" << target->inst->getOpcodeName() << "\n";
      }
      terminateStateOnError(*unbound, "memory error: out of bound pointer", Ptr,
                            NULL, getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state,
                                   const MemoryObject *mo,
                                   const std::string &name) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayKTest) {
    // Find a unique name for this array.  First try the original name,
    // or if that fails try adding a unique identifier.
    unsigned id = 0;
    std::string uniqueName = name;
    while (!state.arrayNames.insert(uniqueName).second) {
      uniqueName = name + "_" + llvm::utostr(++id);
    }
    const Array *array = arrayCache.CreateArray(uniqueName, mo->size);
    if (INTERPOLATION_ENABLED) {
      // We create shadow array as existentially-quantified
      // variables for subsumption checking
      const Array *shadow = arrayCache.CreateArray(
          TxShadowArray::getShadowName(uniqueName), mo->size);
      TxShadowArray::addShadowArrayMap(array, shadow);
      txTree->executeMakeSymbolic(state.prevPC->inst, mo->getBaseExpr(), array);
      if (DebugTracerX)
        llvm::errs() << "[executeMakeSymbolic:executeMakeSymbolic] Node:" << state.txTreeNode->getNodeSequenceNumber() << "\n";
    }

    bindObjectInState(state, mo, false, array);
    state.addSymbolic(mo, array);

    std::map<ExecutionState *, std::vector<SeedInfo> >::iterator it =
        seedMap.find(&state);
    if (it != seedMap.end()) { // In seed mode we need to add this as a
                               // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(),
                                           siie = it->second.end();
           siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, "ran out of inputs during seeding",
                                  User);
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension) &&
                obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
            std::stringstream msg;
            msg << "replace size mismatch: " << mo->name << "[" << mo->size
                << "]"
                << " vs " << obj->name << "[" << obj->numBytes << "]"
                << " in test\n";

            terminateStateOnError(state, msg.str(), User);
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes,
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i = obj->numBytes; i < mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, mo, false);
    if (replayPosition >= replayKTest->numObjects) {
      terminateStateOnError(state, "replay count mismatch", User);
    } else {
      KTestObject *obj = &replayKTest->objects[replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", User);
      } else {
        for (unsigned i = 0; i < mo->size; i++)
          os->write8(i, obj->bytes[i]);
      }
    }
  }
}

/***/

void Executor::runFunctionAsMain(Function *f, int argc, char **argv,
                                 char **envp) {

  std::vector<ref<Expr> > arguments;

  // force deterministic initialization of memory objects
  srand(1);
  srandom(1);

  MemoryObject *argvMO = 0;

  // In order to make uclibc happy and be closer to what the system is
  // doing we lay out the environments at the end of the argv array
  // (both are terminated by a null). There is also a final terminating
  // null that uclibc seems to expect, possibly the ELF header?

  int envc;
  for (envc = 0; envp[envc]; ++envc)
    ;

  unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
  KFunction *kf = kmodule->functionMap[f];
  assert(kf);
  Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
  if (ai != ae) {
    arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));

    if (++ai != ae) {
      argvMO = memory->allocate((argc + 1 + envc + 1 + 1) * NumPtrBytes, false,
                                true, f->begin()->begin());

      if (!argvMO)
        klee_error("Could not allocate memory for function arguments");

      arguments.push_back(argvMO->getBaseExpr());

      if (++ai != ae) {
        uint64_t envp_start = argvMO->address + (argc + 1) * NumPtrBytes;
        arguments.push_back(Expr::createPointer(envp_start));

        if (++ai != ae)
          klee_error("invalid main function (expect 0-3 arguments)");
      }
    }
  }

  ExecutionState *state = new ExecutionState(kmodule->functionMap[f]);

  if (pathWriter)
    state->pathOS = pathWriter->open();
  if (symPathWriter)
    state->symPathOS = symPathWriter->open();

  if (statsTracker)
    statsTracker->framePushed(*state, 0);

  assert(arguments.size() == f->arg_size() && "wrong number of arguments");
  for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
    bindArgument(kf, i, *state, arguments[i]);

  if (argvMO) {
    ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

    for (int i = 0; i < argc + 1 + envc + 1 + 1; i++) {
      if (i == argc || i >= argc + 1 + envc) {
        // Write NULL pointer
        argvOS->write(i * NumPtrBytes, Expr::createPointer(0));
      } else {
        char *s = i < argc ? argv[i] : envp[i - (argc + 1)];
        int j, len = strlen(s);

        MemoryObject *arg =
            memory->allocate(len + 1, false, true, state->pc->inst);
        if (!arg)
          klee_error("Could not allocate memory for function arguments");
        ObjectState *os = bindObjectInState(*state, arg, false);
        for (j = 0; j < len + 1; j++)
          os->write8(j, s[j]);

        // Write pointer to newly allocated and initialised argv/envp c-string
        argvOS->write(i * NumPtrBytes, arg->getBaseExpr());
      }
    }
  }

  initializeGlobals(*state);

  processTree = new PTree(state);
  state->ptreeNode = processTree->root;

  if (INTERPOLATION_ENABLED) {
    txTree = new TxTree(state, kmodule->targetData, &globalAddresses);
    state->txTreeNode = txTree->root;
    TxTreeGraph::initialize(txTree->root);
    if (DebugTracerX)
      llvm::errs() << "[runFunctionAsMain:initialize]\n";
  }

  run(*state);
  delete processTree;
  processTree = 0;

  if (INTERPOLATION_ENABLED) {
    TxTreeGraph::save(interpreterHandler->getOutputFilename("tree.dot"));
    TxTreeGraph::deallocate();
    if (DebugTracerX)
      llvm::errs() << "[runFunctionAsMain:save]\n";

    delete txTree;
    txTree = 0;

#ifdef ENABLE_Z3
    // Print interpolation time statistics
    interpreterHandler->assignSubsumptionStats(TxTree::getInterpolationStat());
#endif
  }

  if (SpecTypeToUse != NO_SPEC) {
    std::string outSpecFile = interpreterHandler->getOutputFilename("spec.txt");
    std::ofstream outSpec(outSpecFile.c_str(), std::ofstream::app);

    outSpec << "Total Independence Yes: " << independenceYes << "\n";
    outSpec << "Total Independence No: " << independenceNo << "\n";

    if (SpecStrategyToUse == AGGRESSIVE) {
      outSpec << "Total Independence No & Success: "
              << (independenceNo - specFail) << "\n";
      outSpec << "Total Independence No & Fail: " << specFail << "\n";
    } else if (SpecStrategyToUse == CUSTOM) {
      outSpec << "Total Dynamic Yes: " << dynamicYes << "\n";
      outSpec << "Total Dynamic No: " << dynamicNo << "\n";
      outSpec << "Total Independence No, Dynamic Yes & Success: "
              << (dynamicYes - specFail) << "\n";
      outSpec << "Total Independence No, Dynamic Yes & Fail: " << specFail
              << "\n";
    }

    unsigned int statsTrackerTotal = 0;
    unsigned int statsTrackerFail = 0;
    unsigned int statsTrackerSucc = 0;
    for (std::map<llvm::BasicBlock *, std::vector<unsigned int> >::iterator
             it = StatsTracker::bbSpecCount.begin(),
             ie = StatsTracker::bbSpecCount.end();
         it != ie; ++it) {
      statsTrackerTotal += it->second[0];
      statsTrackerFail += it->second[1];
      statsTrackerSucc += it->second[2];
    }
    outSpec << "StatsTracker Total: " << statsTrackerTotal << "\n";
    outSpec << "StatsTracker Fail: " << statsTrackerFail << "\n";
    outSpec << "StatsTracker Success: " << statsTrackerSucc << "\n";

    // total fail
    // fail because of new BBs
    unsigned int failNew = 0;
    for (std::map<uintptr_t, unsigned int>::iterator it = specFailNew.begin(),
                                                     ie = specFailNew.end();
         it != ie; ++it) {
      failNew += it->second;
    }
    // fail because of revisted BBs
    unsigned int failRevisited = 0;
    for (std::map<uintptr_t, unsigned int>::iterator it = specRevisited.begin(),
                                                     ie = specRevisited.end();
         it != ie; ++it) {
      failRevisited += it->second;
    }

    // fail & no interpolant
    // fail because of new BBs
    unsigned int failNewNoInter = 0;
    for (std::map<uintptr_t, unsigned int>::iterator
             it = specFailNoInter.begin(),
             ie = specFailNoInter.end();
         it != ie; ++it) {
      failNewNoInter += it->second;
    }
    // fail because of revisted BBs
    unsigned int failRevisitedNoInter = 0;
    for (std::map<uintptr_t, unsigned int>::iterator
             it = specRevisitedNoInter.begin(),
             ie = specRevisitedNoInter.end();
         it != ie; ++it) {
      failRevisitedNoInter += it->second;
    }

    outSpec << "Total speculation failures because of New BB: " << failNew
            << "\n";
    outSpec << "Total speculation failures because of New BB with no "
               "interpolation: " << failNewNoInter << "\n";

    outSpec << "Total speculation failures because of Revisted: "
            << failRevisited << "\n";
    outSpec << "Total speculation failures because of Revisted with no "
               "interpolation: " << failRevisitedNoInter << "\n";

    outSpec << "Total speculation failures because of Bug Hit: "
            << (specFail - failNew - failRevisited) << "\n";

    outSpec << "Total speculation fail time: "
            << totalSpecFailTime / double(CLOCKS_PER_SEC) << "\n";

    // print frequency of failure at each program point
    outSpec << "Frequency of failures because New BB with no interpolation:\n";
    for (std::map<uintptr_t, unsigned int>::iterator
             it = specFailNoInter.begin(),
             ie = specFailNoInter.end();
         it != ie; ++it) {
      outSpec << it->first << ": " << it->second << "\n";
    }
    outSpec
        << "Frequency of failures because Revisted with no interpolation:\n";
    for (std::map<uintptr_t, unsigned int>::iterator
             it = specRevisitedNoInter.begin(),
             ie = specRevisitedNoInter.end();
         it != ie; ++it) {
      outSpec << it->first << ": " << it->second << "\n";
    }
  }

  if (BBCoverage >= 1) {
    llvm::errs()
        << "************Basic Block Coverage Report Starts****************"
        << "\n";
    interpreterHandler->getInfoStream()
        << "KLEE: done: Total number of single time Visited Basic Blocks: "
        << visitedBlocks.size() << "\n";
    interpreterHandler->getInfoStream()
        << "KLEE: done: Total number of Basic Blocks: " << allBlockCount
        << "\n";
    llvm::errs()
        << "KLEE: done: Total number of single time Visited Basic Blocks: "
        << visitedBlocks.size() << "\n";
    llvm::errs() << "KLEE: done: Total number of Basic Blocks: "
                 << allBlockCount << "\n";
    llvm::errs()
        << "************Basic Block Coverage Report Ends****************"
        << "\n";
  }
  if (BBCoverage >= 2) {
    // VisitedBB.txt
    std::string visitedBBFile =
        interpreterHandler->getOutputFilename("VisitedBB.txt");
    std::ofstream visitedBBFileOut(visitedBBFile.c_str(), std::ofstream::app);

    for (std::set<llvm::BasicBlock *>::iterator it = visitedBlocks.begin(),
                                                ie = visitedBlocks.end();
         it != ie; ++it) {

      int order = fBBOrder[(*it)->getParent()][*it];
      std::string functionName = ((*it)->getParent())->getName();
      visitedBBFileOut << order << "\n";
    }

    visitedBBFileOut.close();
  }

  if (BBCoverage >= 4) {
    llvm::errs() << "************ICMP/Atomic Condition Coverage Report "
                    "Starts****************"
                 << "\n";
    interpreterHandler->getInfoStream()
        << "KLEE: done: Total number of Covered ICMP/Atomic Condition: "
        << coveredICMPCount << "\n";
    interpreterHandler->getInfoStream()
        << "KLEE: done: Total number of All ICMP/Atomic Conditions "
        << allICMPCount << "\n";
    llvm::errs()
        << "KLEE: done: Total number of Covered ICMP/Atomic Condition: "
        << coveredICMPCount << "\n";
    llvm::errs() << "KLEE: done: Total number of All ICMP/Atomic Condition: "
                 << allICMPCount << "\n";
    llvm::errs() << "************ICMP/Atomic Condition Coverage Report "
                    "Ends****************"
                 << "\n";
  }
  // hack to clear memory objects
  delete memory;
  memory = new MemoryManager(NULL);

  globalObjects.clear();
  globalAddresses.clear();

  if (statsTracker)
    statsTracker->done();

  if (atMemoryLimit)
    klee_warning("Memory cap exceeded!!!\n");
  else
    klee_message("Memory cap NOT exceeded!\n");
}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state, std::string &res,
                                Interpreter::LogType logFormat) {

  std::ostringstream info;

  switch (logFormat) {
  case STP: {
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } break;

  case KQUERY: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprPPrinter::printConstraints(info, state.constraints);
    res = info.str();
  } break;

  case SMTLIB2: {
    std::string Str;
    llvm::raw_string_ostream info(Str);
    ExprSMTLIBPrinter printer;
    printer.setOutput(info);
    Query query(state.constraints, ConstantExpr::alloc(0, Expr::Bool));
    printer.setQuery(query);
    printer.generateOutput();
    res = info.str();
  } break;

  default:
    klee_warning("Executor::getConstraintLog() : Log format not supported!");
  }
}

bool Executor::getSymbolicSolution(
    const ExecutionState &state,
    std::vector<std::pair<std::string, std::vector<unsigned char> > > &res) {
  solver->setTimeout(coreSolverTimeout);

  ExecutionState tmp(state);

  // Go through each byte in every test case and attempt to restrict
  // it to the constraints contained in cexPreferences.  (Note:
  // usually this means trying to make it an ASCII character (0-127)
  // and therefore human readable. It is also possible to customize
  // the preferred constraints.  See test/Features/PreferCex.c for
  // an example) While this process can be very expensive, it can
  // also make understanding individual test cases much easier.
  for (unsigned i = 0; i != state.symbolics.size(); ++i) {
    const MemoryObject *mo = state.symbolics[i].first;
    std::vector<ref<Expr> >::const_iterator pi = mo->cexPreferences.begin(),
                                            pie = mo->cexPreferences.end();
    for (; pi != pie; ++pi) {
      bool mustBeTrue;
      // Attempt to bound byte to constraints held in cexPreferences
      bool success =
          solver->mustBeTrue(tmp, Expr::createIsZero(*pi), mustBeTrue);
      // If it isn't possible to constrain this particular byte in the desired
      // way (normally this would mean that the byte can't be constrained to
      // be between 0 and 127 without making the entire constraint list UNSAT)
      // then just continue on to the next byte.
      if (!success)
        break;
      // If the particular constraint operated on in this iteration through
      // the loop isn't implied then add it to the list of constraints.
      if (!mustBeTrue)
        tmp.addConstraint(*pi);
    }
    if (pi != pie)
      break;
  }

  std::vector<std::vector<unsigned char> > values;
  std::vector<const Array *> objects;
  std::vector<ref<Expr> > unsatCore;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values, unsatCore);
  solver->setTimeout(0);
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(llvm::errs(), state.constraints,
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }

  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(
    const ExecutionState &state,
    std::map<const std::string *, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state, ref<Expr> e,
                                            ref<ConstantExpr> value) {

  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; // re->updates.root->object;
      const ObjectState *os = state.addressSpace.findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly &&
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace.getWriteable(mo, os);
        wos->write(CE, it->second);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(LLVM_TYPE_Q llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

///

Interpreter *Interpreter::create(const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(opts, ih);
}
