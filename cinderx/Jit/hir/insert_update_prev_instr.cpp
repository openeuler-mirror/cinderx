// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/insert_update_prev_instr.h"

#include "cinderx/Common/code.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#include <stack>
#include <vector>

namespace jit::hir {

namespace {

class BytecodeIndexToLine {
 public:
  explicit BytecodeIndexToLine(PyCodeObject* co) {
    code_ = co;
    size_t num_indices = countIndices(co);
    indexToLine_.reserve(num_indices);
    PyCodeAddressRange range;
    Cix_PyCode_InitAddressRange(co, &range);
    int idx = 0;
    while (Cix_PyLineTable_NextAddressRange(&range)) {
      if (idx >= num_indices) {
        break;
      }
      JIT_DCHECK(
          range.ar_start % sizeof(_Py_CODEUNIT) == 0,
          "offsets should be a multiple of code-units");
      JIT_DCHECK(
          idx == range.ar_start / 2, "Index does not line up with range");
      for (; idx < range.ar_end / 2; idx++) {
        indexToLine_.emplace_back(range.ar_line);
      }
    }
  }

  int lineNoFor(BCIndex index) const {
    if (index.value() < 0) {
      return -1;
    }
    if (index.value() >= indexToLine_.size()) {
      // test.test_exceptions.PEP626Tests.test_missing_lineno_shows_as_none
      // specifically checks that things work when there isn't enough line
      // number information.
      return -1;
    } else {
      return indexToLine_[index.value()];
    }
  }

  PyCodeObject* code_;

 private:
  std::vector<int> indexToLine_;
};

struct InlineStackState {
  InlineStackState(BasicBlock* block, BeginInlinedFunction* parent) {
    this->block = block;
    this->parent = parent;
  }
  BasicBlock* block;
  BeginInlinedFunction* parent;
};

} // namespace

void InsertUpdatePrevInstr::Run([[maybe_unused]] Function& func) {
  // We can have instructions w/ different code objects when we have
  // inlined functions so we maintain multiple BytecodeIndexToLine based upon
  // the code object
  std::unordered_map<PyCodeObject*, BytecodeIndexToLine> code_bc_idx_map;
  code_bc_idx_map.emplace(func.code, BytecodeIndexToLine(func.code));

  std::stack<InlineStackState> worklist;
  std::unordered_set<BasicBlock*> enqueued;
  std::unordered_map<BeginInlinedFunction*, BeginInlinedFunction*> parents;

  worklist.emplace(func.cfg.entry_block, nullptr);
  [[maybe_unused]] bool inited_once = false;
  while (!worklist.empty()) {
    auto cur = worklist.top();
    auto block = cur.block;
    auto parent = cur.parent;
    worklist.pop();

    int prev_emitted_lno_or_bc = INT_MAX;
#if PY_VERSION_HEX < 0x030C0000
    int prev_published_bc = INT_MAX;
#endif
    Instr* last_emitted = nullptr;
    for (Instr& instr : *block) {
      auto update_one = [&]() {
        auto add_update_prev_instr = [&](int line_no) {
          if (last_emitted != nullptr) {
            last_emitted->unlink();
            static_cast<UpdatePrevInstr*>(last_emitted)->setLineNo(line_no);
            last_emitted->copyBytecodeOffset(instr);
            last_emitted->InsertBefore(instr);
          } else {
            last_emitted = UpdatePrevInstr::create(line_no, parent);
            last_emitted->copyBytecodeOffset(instr);
            last_emitted->InsertBefore(instr);
          }
        };
        // If we don't have a valid line table to optimize with, update after
        // every bytecode.
        bool update_every_bc = func.code->co_linetable == nullptr ||
            PyBytes_Size(func.code->co_linetable) == 0;

        if (update_every_bc) {
          int cur_bc_offs = instr.bytecodeOffset().value();
          if (cur_bc_offs != prev_emitted_lno_or_bc) {
            add_update_prev_instr(-1);
            prev_emitted_lno_or_bc = cur_bc_offs;
          }
        } else {
          auto& cur_bc_idx_to_line = code_bc_idx_map.at(
              parent == nullptr ? func.code : parent->code());
          int cur_line_no =
              cur_bc_idx_to_line.lineNoFor(instr.bytecodeOffset());
          if (cur_line_no != prev_emitted_lno_or_bc) {
            add_update_prev_instr(cur_line_no);
            prev_emitted_lno_or_bc = cur_line_no;
          }
        }
      };

      // Inlined functions have a single entry point and a single exit, so we
      // will encounter the exit by following the successor blocks from the
      // entry.
      if (instr.IsBeginInlinedFunction()) {
        // We need to ensure we have emitted a line number update to the outer
        // function before going to the inlined function, otherwise the runtime
        // will see the outer function has having an incomplete frame and skip
        // it in stack traces.
        update_one();

        auto begin = static_cast<BeginInlinedFunction*>(&instr);
        auto code = begin->code();
        if (code_bc_idx_map.find(code) == code_bc_idx_map.end()) {
          code_bc_idx_map.emplace(code, BytecodeIndexToLine(code));
        }
        parents[begin] = parent;
        parent = begin;
        last_emitted = nullptr;
        prev_emitted_lno_or_bc = INT_MAX;
#if PY_VERSION_HEX < 0x030C0000
        prev_published_bc = INT_MAX;
#endif
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
        inited_once = false;
#endif
      } else if (instr.IsEndInlinedFunction()) {
        parent =
            parents[static_cast<EndInlinedFunction&>(instr).matchingBegin()];
        last_emitted = nullptr;
        prev_emitted_lno_or_bc = INT_MAX;
#if PY_VERSION_HEX < 0x030C0000
        prev_published_bc = INT_MAX;
#endif
      }

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
      // The first LoadEvalBreaker is emitted for the RESUME instruction which
      // indicates when we should update the line number from the instruction
      // - 1 to the first instruction to indicate that the frame is now
      // complete.
      if (!inited_once && instr.IsLoadEvalBreaker()) {
        auto target_code = parent == nullptr ? func.code : parent->code();
        auto& cur_bc_idx_to_line = code_bc_idx_map.at(target_code);
        int line_no = cur_bc_idx_to_line.lineNoFor(
            BCIndex(target_code->_co_firsttraceable));
        Instr* update_instr = UpdatePrevInstr::create(line_no, parent);
        update_instr->setBytecodeOffset(
            BCIndex(target_code->_co_firsttraceable));
        update_instr->InsertBefore(instr);
        last_emitted = update_instr;

        inited_once = true;
      }
#if PY_VERSION_HEX < 0x030F0000
      // Stock CPython 3.14 has no JIT executable reifier callback. Keep
      // instr_ptr current before arbitrary execution so frame APIs observe a
      // complete frame at the right source line.
      else if (hasArbitraryExecution(instr)) {
        update_one();
        last_emitted = nullptr;
      }
#endif
#else
      if (hasArbitraryExecution(instr)) {
#if PY_VERSION_HEX < 0x030C0000
        // A materialized 3.11 frame is observable while arbitrary Python is
        // running. Publishing only when the source line changes leaves
        // prev_instr at the first expression on that line, which gives
        // inspect.stack(), sys._getframe() and co_positions() a stale column.
        //
        // Match the stock interpreter cursor, not merely the HIR origin. Most
        // opcodes are observable at their opcode unit. CALL is different:
        // stock's inlined-Python-call path advances prev_instr across its four
        // cache units before entering the callee, so a callee inspecting its
        // caller observes the last CALL cache. CALL_FUNCTION_EX has no cache
        // on 3.11 and therefore naturally resolves to its opcode unit.
        auto target_code = parent == nullptr ? func.code : parent->code();
        if (target_code != nullptr && instr.bytecodeOffset().value() >= 0) {
          BytecodeInstruction bc_instr{target_code, instr.bytecodeOffset()};
          BCIndex published = bc_instr.opcodeIndex();
          if (bc_instr.opcode() == CALL) {
            published = bc_instr.nextInstrOffset().asIndex() - 1;
          }
          if (published.value() != prev_published_bc) {
            // Force an exact-position store even when this boundary shares a
            // line with the previous one. update_one() still supplies the
            // correct line metadata and dead-store behavior.
            prev_emitted_lno_or_bc = INT_MAX;
            update_one();
            JIT_DCHECK(last_emitted != nullptr, "missing position update");
            last_emitted->setBytecodeOffset(published);
            prev_published_bc = published.value();
          }
        } else {
          update_one();
        }
#else
        update_one();
#endif
        last_emitted = nullptr;
      }
#endif
    }

    // Add the successors to be processed
    auto term = block->GetTerminator();
    for (std::size_t i = 0, n = term->numEdges(); i < n; ++i) {
      BasicBlock* succ = term->successor(i);
      if (!enqueued.contains(succ)) {
        worklist.emplace(succ, parent);
        enqueued.insert(succ);
      }
    }
  }
}

} // namespace jit::hir
