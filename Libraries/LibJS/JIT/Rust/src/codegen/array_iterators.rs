/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the ops that for-of loops over arrays are built from: the
//! creation of array iterators, and steps that read and write their state
//! like `%ArrayIteratorPrototype%.next` does.

use super::Codegen;
use super::slow_path_calls::slow_path_saved_registers;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::ir::NodeId;
use crate::ir::value;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// `CreateArrayIterator`: calls `RuntimeInfo::create_array_iterator`.
    pub(in crate::codegen) fn emit_create_array_iterator(&mut self, node: NodeId) {
        let (iterable, next, output) = (self.input(node, 0), self.input(node, 1), self.output(node));
        let saved = slow_path_saved_registers::<M>()
            .without(output)
            .iter()
            .collect::<Vec<_>>();
        self.emit_save_registers(&saved);
        // NB: Either may be in the other's argument register.
        let arguments: [Gpr; 3] = [M::ARGUMENT_GPRS[0], M::ARGUMENT_GPRS[1], M::ARGUMENT_GPRS[2]];
        let scratch = self.pinned.scratch;
        self.masm.move64(scratch, next);
        self.masm.move64(arguments[1], iterable);
        self.masm.move64(arguments[2], scratch);
        self.masm.move64(arguments[0], self.pinned.vm);
        self.masm.call_absolute(self.runtime.create_array_iterator);
        self.masm.move64(output, M::RETURN_GPRS[0]);
        self.emit_restore_registers(&saved, None);
    }

    /// A branch on `BranchCondition::ArrayIterator`: to `target` where the
    /// object at the input is an array iterator over values, or where it is
    /// not, with `invert`.
    pub(in crate::codegen) fn emit_branch_on_array_iterator(&mut self, object: Gpr, target: Label, invert: bool) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let other = self.masm.new_label();
        let fail = if invert { target } else { other };
        // NB: Cells start with the pointer to their class.
        self.masm.load64(scratch, &Address::new(object, 0));
        self.masm
            .branch64_imm(Condition::NotEqual, scratch, layout.array_iterator_class as i64, fail);
        self.masm
            .load8(scratch, &Address::new(object, layout.array_iterator_kind as i32));
        self.masm.branch32_imm(
            Condition::NotEqual,
            scratch,
            i32::from(layout.array_iterator_kind_value),
            fail,
        );
        if !invert {
            self.masm.jump(target);
        }
        self.masm.bind(other);
    }

    /// `LoadArrayIteratorArray`.
    pub(in crate::codegen) fn emit_load_array_iterator_array(&mut self, node: NodeId) {
        let (iterator, output) = (self.input(node, 0), self.output(node));
        self.masm.load64(
            output,
            &Address::new(iterator, self.runtime.layout.array_iterator_array as i32),
        );
    }

    /// `LoadArrayIteratorIndex`: the low 32 bits of the index.
    pub(in crate::codegen) fn emit_load_array_iterator_index(&mut self, node: NodeId) {
        let (iterator, output) = (self.input(node, 0), self.output(node));
        self.masm.load32(
            output,
            &Address::new(iterator, self.runtime.layout.array_iterator_index as i32),
        );
    }

    /// `StoreArrayIteratorIndex`: the non-negative index, zero-extended.
    pub(in crate::codegen) fn emit_store_array_iterator_index(&mut self, node: NodeId) {
        let (iterator, index) = (self.input(node, 0), self.input(node, 1));
        let scratch = self.pinned.scratch;
        self.masm.move32(scratch, index);
        self.masm.store64(
            &Address::new(iterator, self.runtime.layout.array_iterator_index as i32),
            scratch,
        );
    }

    /// `FinishArrayIterator`.
    pub(in crate::codegen) fn emit_finish_array_iterator(&mut self, node: NodeId) {
        let iterator = self.input(node, 0);
        let scratch = self.pinned.scratch;
        self.masm.move_imm64(scratch, value::UNDEFINED);
        self.masm.store64(
            &Address::new(iterator, self.runtime.layout.array_iterator_array as i32),
            scratch,
        );
    }
}
