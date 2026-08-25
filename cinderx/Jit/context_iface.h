// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace jit {

class alignas(16) CodeRuntime;

class IJitContext {
 public:
  IJitContext() {}
  virtual ~IJitContext() = default;

  virtual CodeRuntime* lookupCodeRuntime(
      BorrowedRef<PyFunctionObject> func) = 0;

  // Whether this context's slab storage contains the given runtime.  A
  // retired artifact has no owner link left at destruction time, so it can
  // only hand its runtime's storage back after proving the module context
  // actually owns the slot.
  virtual bool ownsCodeRuntime(const CodeRuntime* runtime) const = 0;

  // Hand a cleared CodeRuntime husk back to this context's slab for reuse.
  virtual void recycleCodeRuntime(CodeRuntime* runtime) = 0;

  virtual BorrowedRef<> zero() = 0;
};

} // namespace jit
