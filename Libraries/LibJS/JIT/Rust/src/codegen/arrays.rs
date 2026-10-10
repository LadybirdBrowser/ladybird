/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the array ops that `Array.prototype.push` of one value is
//! built from, the way its own fast path for packed arrays appends.

use super::Codegen;
use super::slow_path_calls::slow_path_saved_registers;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// `CheckAppendableArray`: exits unless the input is an array that
    /// `Array.prototype.push` appends to without any observable step.
    pub(in crate::codegen) fn emit_check_appendable_array(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let scratch = self.pinned.scratch;
        let offsets = self.runtime.offsets;
        let layout = self.runtime.layout;
        let receiver = self.input(node, 0);
        let (array, prototype) = (self.temp(node, 0), self.temp(node, 1));

        self.unbox_object_or_branch(array, receiver, exit);
        self.masm
            .load16(scratch, &Address::new(array, offsets.object_flags as i32));
        let flags = layout.object_flag_has_magical_length | layout.object_flag_is_extensible;
        self.masm
            .and32_imm(scratch, scratch, u32::from(flags | layout.object_flag_may_interfere));
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(flags), exit);
        self.masm
            .load8(scratch, &Address::new(array, layout.array_is_proxy_target as i32));
        self.masm.branch_test32(Condition::NonZero, scratch, 0xFF, exit);
        self.masm
            .load8(scratch, &Address::new(array, layout.array_length_writable as i32));
        self.masm.branch_test32(Condition::Zero, scratch, 0xFF, exit);
        // Packed indexed storage, or none yet.
        let known_kind = self.masm.new_label();
        self.masm
            .load8(scratch, &Address::new(array, layout.object_indexed_storage_kind as i32));
        self.masm.branch32_imm(
            Condition::Equal,
            scratch,
            i32::from(layout.indexed_storage_kind_packed),
            known_kind,
        );
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.indexed_storage_kind_none),
            exit,
        );
        self.masm.bind(known_kind);

        // The realm's default prototype chain, without indexed properties.
        // NB: %Object.prototype% is an immutable prototype exotic object,
        //     so the chain ends there.
        self.branch_unless_plain_prototype(array, self.runtime.array_prototype.0, prototype, exit);
        self.branch_unless_plain_prototype(prototype, self.runtime.object_prototype.0, array, exit);
    }

    /// `LoadElementsCapacity`: the capacity of packed or holey indexed
    /// storage, and 0 without storage.
    pub(in crate::codegen) fn emit_load_elements_capacity(&mut self, node: NodeId) {
        let (object, output, elements) = (self.input(node, 0), self.output(node), self.temp(node, 0));
        let layout = self.runtime.layout;
        let none = self.masm.new_label();
        let done = self.masm.new_label();
        self.masm
            .load64(elements, &Address::new(object, layout.object_indexed_elements as i32));
        self.masm.branch_test64(Condition::Zero, elements, u64::MAX, none);
        self.masm
            .load32(output, &Address::new(elements, layout.indexed_elements_capacity));
        // NB: The new number of elements must be an int32 too.
        self.masm.branch32_imm(Condition::GreaterThanOrEqual, output, 0, done);
        self.masm.move_imm32(output, i32::MAX as u32);
        self.masm.jump(done);
        self.masm.bind(none);
        self.masm.move_imm32(output, 0);
        self.masm.bind(done);
    }

    /// `AppendElement`: stores the value at the index of the object's
    /// number of elements, below their capacity, and counts it, which makes
    /// the new number the output.
    pub(in crate::codegen) fn emit_append_element_node(&mut self, node: NodeId) {
        let (object, index, value) = (self.input(node, 0), self.input(node, 1), self.input(node, 2));
        let (elements, output, scratch) = (self.temp(node, 0), self.output(node), self.pinned.scratch);
        let layout = self.runtime.layout;
        let size = Address::new(object, layout.object_indexed_array_like_size as i32);
        self.masm.move32(output, index);
        self.masm.add32_imm(output, output, 1);
        self.masm.store32(&size, output);
        self.masm
            .load64(elements, &Address::new(object, layout.object_indexed_elements as i32));
        self.masm.move32(scratch, index);
        self.masm
            .store64(&Address::indexed(elements, scratch, Scale::Eight, 0), value);
    }

    /// Puts the prototype of `object` in `dst`, or branches to `fail`
    /// unless it is `expected` and has no indexed properties.
    pub(super) fn branch_unless_plain_prototype(&mut self, object: Gpr, expected: u64, dst: Gpr, fail: Label) {
        let scratch = self.pinned.scratch;
        let offsets = self.runtime.offsets;
        let layout = self.runtime.layout;
        self.masm
            .load64(scratch, &Address::new(object, offsets.object_shape as i32));
        self.masm
            .load64(scratch, &Address::new(scratch, layout.shape_prototype as i32));
        self.masm.move_imm64(dst, expected);
        self.masm.branch64(Condition::NotEqual, scratch, dst, fail);
        self.masm.load32(
            scratch,
            &Address::new(dst, layout.object_indexed_array_like_size as i32),
        );
        self.masm.branch_test32(Condition::NonZero, scratch, u32::MAX, fail);
        self.masm
            .load16(scratch, &Address::new(dst, offsets.object_flags as i32));
        self.masm.branch_test32(
            Condition::NonZero,
            scratch,
            u32::from(layout.object_flag_may_interfere),
            fail,
        );
    }

    /// `CallArrayPush`: calls `RuntimeInfo::array_push`, which grows the
    /// array's elements, saving the registers the call clobbers.
    pub(in crate::codegen) fn emit_call_array_push(&mut self, node: NodeId) {
        let scratch = self.pinned.scratch;
        let (receiver, value, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let saved = slow_path_saved_registers::<M>()
            .without(output)
            .iter()
            .collect::<Vec<_>>();
        self.emit_save_registers(&saved);
        // NB: Either may be in the other's argument register.
        let arguments: [Gpr; 2] = [M::ARGUMENT_GPRS[0], M::ARGUMENT_GPRS[1]];
        self.masm.move64(scratch, receiver);
        self.masm.move64(arguments[1], value);
        self.emit_unbox_cell(arguments[0], scratch);
        self.masm.call_absolute(self.runtime.array_push);
        self.masm.move64(output, M::RETURN_GPRS[0]);
        self.emit_restore_registers(&saved, None);
    }
}
