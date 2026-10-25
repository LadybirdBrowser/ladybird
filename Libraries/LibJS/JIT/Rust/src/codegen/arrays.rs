/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of the array ops that `Array.prototype.push` of one value is
//! built from, the way its own fast path for packed arrays appends.

use super::slow_path_calls::slow_path_saved_registers;
use super::{Codegen, InputValue};
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::ir::NodeId;

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Calls the splice helper with the inserted values on the native
    /// stack. The receiver, start and delete count are in argument registers;
    /// store the other inputs before setting up the remaining arguments.
    pub(in crate::codegen) fn emit_call_array_splice(&mut self, node: NodeId) {
        let count = self.graph.node(node).inputs.len() - 3;
        let bytes = ((count * 8 + 15) & !15) as i64;
        if bytes != 0 {
            self.masm.sub64_imm(M::STACK_POINTER, M::STACK_POINTER, bytes);
        }
        for index in 0..count {
            let address = Address::new(M::STACK_POINTER, (index * 8) as i32);
            match self.input_value(node, index + 3) {
                InputValue::Register(value) => self.masm.store64(&address, value),
                InputValue::Constant(value) => self.masm.store_imm64(&address, value),
            }
        }
        self.masm.move64(M::ARGUMENT_GPRS[0], self.pinned.vm);
        self.masm.move64(M::ARGUMENT_GPRS[4], M::STACK_POINTER);
        self.masm.move_imm32(M::ARGUMENT_GPRS[5], count as u32);
        self.masm.call_absolute(self.runtime.array_splice);
        if bytes != 0 {
            self.masm.add64_imm(M::STACK_POINTER, M::STACK_POINTER, bytes);
        }
    }

    /// `CheckAppendableArray`: exits unless the input is an array that
    /// `Array.prototype.push` appends to without any observable step.
    pub(in crate::codegen) fn emit_check_appendable_array(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let receiver = self.input(node, 0);
        let (array, prototype) = (self.temp(node, 0), self.temp(node, 1));

        self.unbox_object_or_branch(array, receiver, exit);
        self.branch_unless_plain_array(array, prototype, exit);
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
    }

    /// Branches to `fail` unless the object at `array` is an array that
    /// can get a new element without any observable step: an extensible
    /// array that does not interfere with indexed access, no proxy target,
    /// whose prototype chain is the realm's default one without indexed
    /// properties. Keeps `array`, and clobbers `prototype`.
    pub(super) fn branch_unless_plain_array(&mut self, array: Gpr, prototype: Gpr, fail: Label) {
        let scratch = self.pinned.scratch;
        let offsets = self.runtime.offsets;
        let layout = self.runtime.layout;
        self.masm
            .load16(scratch, &Address::new(array, offsets.object_flags as i32));
        let flags = layout.object_flag_has_magical_length | layout.object_flag_is_extensible;
        self.masm
            .and32_imm(scratch, scratch, u32::from(flags | layout.object_flag_may_interfere));
        self.masm
            .branch32_imm(Condition::NotEqual, scratch, i32::from(flags), fail);
        self.masm
            .load8(scratch, &Address::new(array, layout.array_is_proxy_target as i32));
        self.masm.branch_test32(Condition::NonZero, scratch, 0xFF, fail);

        // NB: %Object.prototype% is an immutable prototype exotic object,
        //     so the chain ends there.
        self.branch_unless_plain_prototype(array, self.runtime.array_prototype.0, prototype, fail);
        self.branch_unless_plain_prototype(prototype, self.runtime.object_prototype.0, prototype, fail);
    }

    /// A branch on `BranchCondition::PlainArrayPrototypeChain`: to `target`
    /// where the object at `object` is an array whose prototype chain is the
    /// realm's default one, without indexed properties, or where it is not,
    /// with `invert`.
    pub(in crate::codegen) fn emit_branch_on_plain_array_prototype_chain(
        &mut self,
        node: NodeId,
        object: Gpr,
        target: Label,
        invert: bool,
    ) {
        let prototype = self.temp(node, 0);
        let scratch = self.pinned.scratch;
        let other = self.masm.new_label();
        let fail = if invert { target } else { other };
        self.masm
            .load16(scratch, &Address::new(object, self.runtime.offsets.object_flags as i32));
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(self.runtime.layout.object_flag_has_magical_length),
            fail,
        );
        // NB: %Object.prototype% is an immutable prototype exotic object,
        //     so the chain ends there.
        self.branch_unless_plain_prototype(object, self.runtime.array_prototype.0, prototype, fail);
        self.branch_unless_plain_prototype(prototype, self.runtime.object_prototype.0, prototype, fail);
        if !invert {
            self.masm.jump(target);
        }
        self.masm.bind(other);
    }

    /// A branch on `BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties`:
    /// to `target` where the object at `object` is an array whose shape has
    /// the realm's `%Array.prototype%` as its prototype and no named
    /// properties, or where it is not, with `invert`.
    pub(in crate::codegen) fn emit_branch_on_array_with_realm_array_prototype_and_no_named_properties(
        &mut self,
        node: NodeId,
        object: Gpr,
        target: Label,
        invert: bool,
    ) {
        let shape = self.temp(node, 0);
        let scratch = self.pinned.scratch;
        let offsets = self.runtime.offsets;
        let layout = self.runtime.layout;
        let other = self.masm.new_label();
        let fail = if invert { target } else { other };
        self.masm
            .load16(scratch, &Address::new(object, offsets.object_flags as i32));
        self.masm.branch_test32(
            Condition::Zero,
            scratch,
            u32::from(layout.object_flag_has_magical_length),
            fail,
        );
        self.masm
            .load64(shape, &Address::new(object, offsets.object_shape as i32));
        self.masm
            .load64(scratch, &Address::new(shape, layout.shape_prototype as i32));
        self.masm.branch64_imm(
            Condition::NotEqual,
            scratch,
            self.runtime.array_prototype.0 as i64,
            fail,
        );
        self.masm
            .load32(scratch, &Address::new(shape, layout.shape_property_count as i32));
        self.masm.branch_test32(Condition::NonZero, scratch, u32::MAX, fail);
        if !invert {
            self.masm.jump(target);
        }
        self.masm.bind(other);
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
    /// unless it is `expected` and has no indexed properties. `dst` may be
    /// `object`.
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

    /// `CallStoreHoleyElement`: calls `RuntimeInfo::store_holey_element`,
    /// saving the registers the call clobbers.
    pub(in crate::codegen) fn emit_call_store_holey_element(&mut self, node: NodeId) {
        let saved = slow_path_saved_registers::<M>().iter().collect::<Vec<_>>();
        self.emit_save_registers(&saved);
        // NB: The inputs may be in each other's argument registers, which the saves area holds the values of.
        for (input, argument) in M::ARGUMENT_GPRS.iter().take(3).enumerate() {
            let value = self.input(node, input);
            match saved.iter().position(|register| *register == value) {
                Some(index) => {
                    let address = self.save_address(index);
                    self.masm.load64(*argument, &address);
                }
                None => self.masm.move64(*argument, value),
            }
        }
        self.masm.call_absolute(self.runtime.store_holey_element);
        self.emit_restore_registers(&saved, None);
    }
}
