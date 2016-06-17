//===-- Constraints.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CONSTRAINTS_H
#define KLEE_CONSTRAINTS_H

#include "klee/Expr.h"

// FIXME: Currently we use ConstraintManager for two things: to pass
// sets of constraints around, and to optimize constraints. We should
// move the first usage into a separate data structure
// (ConstraintSet?) which ConstraintManager could embed if it likes.
namespace klee {

class ExprVisitor;
  
class ConstraintManager {
public:
  typedef std::vector< ref<Expr> > constraints_ty;
  typedef constraints_ty::iterator iterator;
  typedef constraints_ty::const_iterator const_iterator;

  ConstraintManager() {}

  // create from constraints with no optimization
  explicit
  ConstraintManager(const std::vector< ref<Expr> > &_constraints) :
    constraints(_constraints) {}

  ConstraintManager(const ConstraintManager &cs) : constraints(cs.constraints) {}

  typedef std::vector< ref<Expr> >::const_iterator constraint_iterator;

  // given a constraint which is known to be valid, attempt to 
  // simplify the existing constraint set
  void simplifyForValidConstraint(ref<Expr> e);

  ref<Expr> simplifyExpr(ref<Expr> e) const;

  void addConstraint(ref<Expr> e);

  /// Replace the state constraint that has variable intersection with the
  /// condition in klee_abstract() and collect constraints that are kept (not
  /// removed/replaced) which later use for construction of a new PathCondition.
  ///
  /// \param e Condition in klee_abstract()
  /// \param keptConstraints Constraints that are kept (not removed/replaced)
  void abstractConstraints(ref<Expr> e,
                           std::vector<ref<Expr> > &keptConstraints);

  static void getArrayFromExpr(ref<Expr> expr,
                               std::set<const Array *> &arrayPack);

  static bool variablesIntersect(std::set<const Array *> &v1,
                                 std::set<const Array *> &v2);

  bool empty() const {
    return constraints.empty();
  }
  ref<Expr> back() const {
    return constraints.back();
  }
  constraint_iterator begin() const {
    return constraints.begin();
  }
  constraint_iterator end() const {
    return constraints.end();
  }
  size_t size() const {
    return constraints.size();
  }

  bool operator==(const ConstraintManager &other) const {
    return constraints == other.constraints;
  }
  
  constraints_ty getConstraints() const{
	  return constraints;
  }

private:
  std::vector< ref<Expr> > constraints;

  // returns true iff the constraints were modified
  bool rewriteConstraints(ExprVisitor &visitor);

  void addConstraintInternal(ref<Expr> e);
};

}

#endif /* KLEE_CONSTRAINTS_H */
