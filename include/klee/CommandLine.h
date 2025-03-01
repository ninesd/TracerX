/*
 * This header groups command line options declarations and associated data
 * that is common for klee and kleaver.
 */

#ifndef KLEE_COMMANDLINE_H
#define KLEE_COMMANDLINE_H

#include "llvm/Support/CommandLine.h"
#include "klee/Config/config.h"

#ifdef ENABLE_Z3
#ifdef ENABLE_STP
#define INTERPOLATION_ENABLED (CoreSolverToUse == Z3_SOLVER && !NoInterpolation)
#else
#define INTERPOLATION_ENABLED (!NoInterpolation)
#endif
#define OUTPUT_INTERPOLATION_TREE (INTERPOLATION_ENABLED &&OutputTree)
#else
#define INTERPOLATION_ENABLED false
#define OUTPUT_INTERPOLATION_TREE false
#endif

namespace klee {

extern llvm::cl::opt<bool> UseFastCexSolver;

extern llvm::cl::opt<bool> UseCexCache;

extern llvm::cl::opt<bool> UseCache;

extern llvm::cl::opt<bool> UseIndependentSolver;

extern llvm::cl::opt<bool> DebugValidateSolver;

extern llvm::cl::opt<int> MinQueryTimeToLog;

extern llvm::cl::opt<double> MaxCoreSolverTime;

extern llvm::cl::opt<bool> UseForkedCoreSolver;

extern llvm::cl::opt<bool> CoreSolverOptimizeDivides;

/// The different query logging solvers that can switched on/off
enum QueryLoggingSolverType {
  ALL_PC,     ///< Log all queries (un-optimised) in .pc (KQuery) format
  ALL_SMTLIB, ///< Log all queries (un-optimised)  .smt2 (SMT-LIBv2) format
  SOLVER_PC,  ///< Log queries passed to solver (optimised) in .pc (KQuery)
  /// format
  SOLVER_SMTLIB ///< Log queries passed to solver (optimised) in .smt2
                ///(SMT-LIBv2) format
};

/* Using cl::list<> instead of cl::bits<> results in quite a bit of ugliness
 * when it comes to checking
 * if an option is set. Unfortunately with gcc4.7 cl::bits<> is broken with
 * LLVM2.9 and I doubt everyone
 * wants to patch their copy of LLVM just for these options.
 */
extern llvm::cl::list<QueryLoggingSolverType> queryLoggingOptions;

enum CoreSolverType {
  STP_SOLVER,
  METASMT_SOLVER,
  DUMMY_SOLVER,
  Z3_SOLVER,
  NO_SOLVER
};

enum SpecType {
  NO_SPEC,
  SAFETY,
  COVERAGE
};

enum SpecStrategy {
  TIMID,
  AGGRESSIVE,
  CUSTOM
};

extern llvm::cl::opt<CoreSolverType> CoreSolverToUse;

extern llvm::cl::opt<CoreSolverType> DebugCrossCheckCoreSolverWith;

// We should compile in this option even when ENABLE_Z3
// was undefined to avoid regression test failure.
extern llvm::cl::opt<bool> NoInterpolation;

#ifdef ENABLE_Z3

extern llvm::cl::opt<bool> OutputTree;

extern llvm::cl::opt<bool> SubsumedTest;

extern llvm::cl::opt<bool> NoExistential;

extern llvm::cl::opt<int> MaxFailSubsumption;

extern llvm::cl::opt<int> DebugState;

extern llvm::cl::opt<int> DebugSubsumption;

extern llvm::cl::opt<int> BBCoverage;

extern llvm::cl::opt<bool> ExactAddressInterpolant;

extern llvm::cl::opt<bool> SpecialFunctionBoundInterpolation;

extern llvm::cl::opt<bool> TracerXPointerError;

extern llvm::cl::opt<bool> EmitAllErrorsInSamePath;

extern llvm::cl::opt<SpecType> SpecTypeToUse;

extern llvm::cl::opt<SpecStrategy> SpecStrategyToUse;

extern llvm::cl::opt<std::string> DependencyFolder;

extern llvm::cl::opt<bool> WPInterpolant;

extern llvm::cl::opt<bool> MarkGlobal;

extern llvm::cl::opt<bool> DebugTracerX;

#endif

#ifdef ENABLE_METASMT

enum MetaSMTBackendType {
  METASMT_BACKEND_STP,
  METASMT_BACKEND_Z3,
  METASMT_BACKEND_BOOLECTOR
};

extern llvm::cl::opt<klee::MetaSMTBackendType> MetaSMTBackend;

#endif /* ENABLE_METASMT */

// A bit of ugliness so we can use cl::list<> like cl::bits<>, see
// queryLoggingOptions
template <typename T>
static bool optionIsSet(llvm::cl::list<T> list, T option) {
  return std::find(list.begin(), list.end(), option) != list.end();
}
}

#endif /* KLEE_COMMANDLINE_H */
