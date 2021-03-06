/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RedundantMoveEliminationPass.h"

#include <boost/optional.hpp>

#include "AliasedRegisters.h"
#include "ControlFlow.h"
#include "IRInstruction.h"
#include "Walkers.h"

/**
 * This pass eliminates writes to registers that already hold the written value.
 *
 * For example,
 *   move-object/from16 v0, v33
 *   iget-object v2, v0, LX/04b;.a:Landroid/content/Context; // field@05d6
 *   move-object/from16 v0, v33
 *   iget-object v3, v0, LX/04b;.b:Ljava/lang/String; // field@05d7
 *   move-object/from16 v0, v33
 *   iget-object v4, v0, LX/04b;.c:LX/04K; // field@05d8
 *   move-object/from16 v0, v33
 *
 * It keeps moving v33 to v0 even though they hold the same object!
 *
 * This optimization transforms the above code to this:
 *   move-object/from16 v0, v33
 *   iget-object v2, v0, LX/04b;.a:Landroid/content/Context; // field@05d6
 *   iget-object v3, v0, LX/04b;.b:Ljava/lang/String; // field@05d7
 *   iget-object v4, v0, LX/04b;.c:LX/04K; // field@05d8
 *
 * It does so by examinining all the writes to registers in a basic block, if vA
 * is moved into vB, then vA and vB are aliases until one of them is written
 * with a different value. Any move between registers that are already aliased
 * is unneccesary. Eliminate them.
 *
 * It also does the same thing with constant loads
 *
 * This optimization also will replace source registers with a representative
 * register (a whole alias group has a single representative) . The reason is
 * that if we use fewer registers, DCE could clean up some more moves after
 * us.
 *
 * Possible additions: (TODO?)
 *   wide registers
 *     I tried it, with registers only it's a tiny help. With
 *     constants it causes spurious verify errors at install time. --jhendrick
 */

namespace {

class RedundantMoveEliminationImpl {
 public:
  RedundantMoveEliminationImpl(
      const std::vector<DexClass*>& scope,
      PassManager& mgr,
      const RedundantMoveEliminationPass::Config& config)
      : m_scope(scope), m_mgr(mgr), m_config(config) {}

  void run() {
    walk_methods(m_scope, [this](DexMethod* m) {
      if (m->get_code()) {
        run_on_method(m);
      }
    });
  }

 private:
  const std::vector<DexClass*>& m_scope;
  PassManager& m_mgr;
  const RedundantMoveEliminationPass::Config& m_config;

  void run_on_method(DexMethod* method) {
    std::vector<IRInstruction*> deletes;

    auto code = method->get_code();
    code->build_cfg();
    const auto& blocks = code->cfg().blocks();

    for (auto block : blocks) {
      run_on_block(block, deletes);
    }

    m_mgr.incr_metric("redundant_moves_eliminated", deletes.size());
    for (auto insn : deletes) {
      code->remove_opcode(insn);
    }
  }

  /*
   * fill the `deletes` vector with redundant instructions
   *
   * An instruction can be removed if we know the source and destination are
   * aliases
   */
  void run_on_block(Block* block, std::vector<IRInstruction*>& deletes) {

    AliasedRegisters aliases;
    for (auto& mei : InstructionIterable(block)) {
      RegisterValue src = get_src_value(mei.insn);
      if (src != RegisterValue::none()) {
        // either a move or a constant load into `dst`
        RegisterValue dst{mei.insn->dest()};
        if (aliases.are_aliases(dst, src)) {
          deletes.push_back(mei.insn);
        } else {
          aliases.break_alias(dst);
          aliases.make_aliased(dst, src);
        }
      } else {
        if (m_config.replace_with_representative) {
          replace_with_representative(mei.insn, aliases);
        }
        if (mei.insn->dests_size() > 0) {
          // dest is being written to but not by a simple move from another
          // register or a constant load. Break its aliases because we don't
          // know what its value is.
          RegisterValue dst{mei.insn->dest()};
          aliases.break_alias(dst);
          if (mei.insn->dest_is_wide()) {
            Register wide_reg = mei.insn->dest() + 1;
            RegisterValue wide{wide_reg};
            aliases.break_alias(wide);
          }
        }
      }
    }
  }

  // Each group of aliases has one representative register.
  // Try to replace source registers with their representative.
  void replace_with_representative(IRInstruction* insn,
                                   AliasedRegisters& aliases) {
    if (insn->srcs_size() > 0 &&
        !opcode::has_range(insn->opcode()) && // range has to stay in order
        // we need to make sure the dest and src of check-cast stay identical,
        // because the dest is simply an alias to the src. See the comments in
        // IRInstruction.h for details.
        insn->opcode() != OPCODE_CHECK_CAST) {
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        RegisterValue val{insn->src(i)};
        boost::optional<Register> rep = aliases.get_representative(val);
        if (rep) {
          // filter out uses of wide registers where the second register isn't
          // also aliased
          if (insn->src_is_wide(i)) {
            RegisterValue orig_wide{(Register)(insn->src(i) + 1)};
            RegisterValue rep_wide{(Register)(*rep + 1)};
            bool wides_are_aliased = aliases.are_aliases(orig_wide, rep_wide);
            if (!wides_are_aliased) {
              continue;
            }
          }

          if (insn->src(i) != *rep) {
            insn->set_src(i, *rep);
            m_mgr.incr_metric("source_regs_replaced_with_representative", 1);
          }
        }
      }
    }
  }

  RegisterValue get_src_value(IRInstruction* insn) {
    switch (insn->opcode()) {
    case OPCODE_MOVE:
    case OPCODE_MOVE_FROM16:
    case OPCODE_MOVE_16:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_OBJECT_FROM16:
    case OPCODE_MOVE_OBJECT_16:
      return RegisterValue{insn->src(0)};
    case OPCODE_CONST:
    case OPCODE_CONST_4:
    case OPCODE_CONST_16:
      if (m_config.eliminate_const_literals) {
        return RegisterValue{insn->literal()};
      } else {
        return RegisterValue::none();
      }
    case OPCODE_CONST_STRING:
    case OPCODE_CONST_STRING_JUMBO: {
      if (m_config.eliminate_const_strings) {
        DexString* str = insn->get_string();
        return RegisterValue{str};
      } else {
        return RegisterValue::none();
      }
    }
    case OPCODE_CONST_CLASS: {
      if (m_config.eliminate_const_classes) {
        DexType* type = insn->get_type();
        return RegisterValue{type};
      } else {
        return RegisterValue::none();
      }
    }
    default:
      return RegisterValue::none();
    }
  }
};
}

void RedundantMoveEliminationPass::run_pass(DexStoresVector& stores,
                                            ConfigFiles& /* unused */,
                                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  RedundantMoveEliminationImpl impl(scope, mgr, m_config);
  impl.run();
  TRACE(RME,
        2,
        "%d redundant moves eliminated\n",
        mgr.get_metric("redundant_moves_eliminated"));
  TRACE(RME,
        2,
        "%d source_regs_replaced_with_representative\n",
        mgr.get_metric("source_regs_replaced_with_representative"));
}

static RedundantMoveEliminationPass s_pass;
