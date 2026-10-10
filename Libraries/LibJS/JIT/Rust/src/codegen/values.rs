/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Helpers that lowerings share for values: their tags, boxing, and
//! branching on their truthiness, and the registers lowerings compute in.

use super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::DoubleCondition;
use crate::asm::Fpr;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::NegativeZero;
use crate::asm::PortableMacroAssembler;
use crate::ir::NodeId;
use crate::ir::value;

/// How many floating point registers lowerings compute with (see
/// `Codegen::lowering_fpr()`).
pub const LOWERING_FPRS: usize = 3;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Floating point register `index` (below `LOWERING_FPRS`), which
    /// lowerings may clobber: the register allocator puts no values there.
    pub(super) fn lowering_fpr(index: usize) -> Fpr {
        debug_assert!(index < LOWERING_FPRS);
        M::ALLOCATABLE_FPRS
            .iter()
            .nth(index)
            .expect("the target has enough floating point registers")
    }

    /// Branches on the truthiness of `value`, which is neither a boolean nor
    /// an int32, whose tag is in the scratch register: to `if_true` or
    /// `if_false` for objects (falsy only if they are `[[IsHTMLDDA]]`),
    /// strings (falsy only if empty), undefined, null and doubles other than
    /// NaN, and to `fallback` for everything else.
    pub(super) fn emit_truthiness_of_other_values(
        &mut self,
        value: Gpr,
        if_true: Label,
        if_false: Label,
        fallback: Label,
    ) {
        let scratch = self.pinned.scratch;
        let not_object = self.masm.new_label();
        let not_string = self.masm.new_label();
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(value::OBJECT_TAG), not_object);
        if self.htmldda_objects_may_exist() {
            self.emit_unbox_cell(scratch, value);
            self.masm.load16(
                scratch,
                &Address::new(scratch, self.runtime.offsets.object_flags as i32),
            );
            self.masm.branch_test32(
                Condition::Zero,
                scratch,
                u32::from(self.runtime.layout.object_flag_is_htmldda),
                if_true,
            );
            self.masm.jump(if_false);
        } else {
            self.masm.jump(if_true);
        }
        self.masm.bind(not_object);
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(value::STRING_TAG), not_string);
        self.emit_unbox_cell(scratch, value);
        self.masm.load32(
            scratch,
            &Address::new(scratch, self.runtime.layout.primitive_string_length as i32),
        );
        self.masm.branch_test32(Condition::NonZero, scratch, u32::MAX, if_true);
        self.masm.jump(if_false);
        self.masm.bind(not_string);
        self.masm.or32_imm(scratch, scratch, 1);
        self.masm
            .branch32_imm(Condition::Equal, scratch, i32::from(value::NULL_TAG), if_false);
        // NB: Doubles are falsy only as +0, -0 and NaN, which goes to the
        //     fallback with the values that are not doubles.
        self.branch_if_not_double(value, fallback);
        self.masm.shl64_imm(scratch, value, 1);
        self.masm.branch_test64(Condition::NonZero, scratch, u64::MAX, if_true);
        self.masm.jump(if_false);
    }

    /// Branches to `target` where the tag of `value` compares to `tag` by
    /// `condition` (`Equal` or `NotEqual`). Leaves the tag in `temp`, which
    /// may be `value`.
    pub(super) fn branch_on_tag(&mut self, condition: Condition, value: Gpr, tag: u16, temp: Gpr, target: Label) {
        self.masm.shr64_imm(temp, value, value::TAG_SHIFT);
        self.masm.branch32_imm(condition, temp, i32::from(tag), target);
    }

    /// Branches to `target` unless `value` is a double other than NaN.
    fn branch_if_not_double(&mut self, value: Gpr, target: Label) {
        let scratch = self.pinned.scratch;
        self.masm.shr64_imm(scratch, value, value::TAG_SHIFT);
        self.masm.and32_imm(scratch, scratch, u32::from(value::BASE_TAG));
        self.masm
            .branch32_imm(Condition::Equal, scratch, i32::from(value::BASE_TAG), target);
    }

    fn box_int32(&mut self, dst: Gpr, integer: Gpr) {
        self.masm.move32(dst, integer);
        self.masm.or64_imm(dst, dst, value::tagged(value::INT32_TAG));
    }

    /// Boxes a double the way `JS::Value(double)` does: as an int32 if it is
    /// one (but not -0), and with NaN canonicalized.
    pub(super) fn box_number(&mut self, dst: Gpr, number: Fpr) {
        let scratch = self.pinned.scratch;
        let not_int32 = self.masm.new_label();
        let nan = self.masm.new_label();
        let done = self.masm.new_label();
        self.masm
            .branch_convert_double_to_int32(scratch, number, not_int32, NegativeZero::Fail);
        self.box_int32(dst, scratch);
        self.masm.jump(done);
        self.masm.bind(not_int32);
        self.masm.branch_double(DoubleCondition::Unordered, number, number, nan);
        self.masm.move_double_to_gpr(dst, number);
        self.masm.jump(done);
        self.masm.bind(nan);
        self.masm.move_imm64(dst, value::CANONICAL_NAN);
        self.masm.bind(done);
    }

    /// Branches to `fail` unless `value` is an object; puts the object's
    /// address in `dst` otherwise.
    pub(super) fn unbox_object_or_branch(&mut self, dst: Gpr, value: Gpr, fail: Label) {
        let scratch = self.pinned.scratch;
        self.branch_on_tag(Condition::NotEqual, value, value::OBJECT_TAG, scratch, fail);
        self.emit_unbox_cell(dst, value);
    }

    pub(super) fn temp(&self, node: NodeId, index: usize) -> Gpr {
        Gpr(self.allocation.node(node).temps[index])
    }
}
