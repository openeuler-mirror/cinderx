// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame_header.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/config.h"

namespace jit {

int frameHeaderSize(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

  return kFrameHeaderOverhead +
      sizeof(PyObject*) * frameSlotsForCodeObject(code);
}

} // namespace jit
