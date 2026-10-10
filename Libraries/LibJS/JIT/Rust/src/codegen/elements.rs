/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lowering of element accesses: checks of the elements kind, bounds and
//! holes, and loads and stores of elements.

use super::Codegen;
use crate::asm::Address;
use crate::asm::Condition;
use crate::asm::Gpr;
use crate::asm::Label;
use crate::asm::PortableMacroAssembler;
use crate::asm::Scale;
use crate::code::Repr;
use crate::ir::ElementsKind;
use crate::ir::NodeId;
use crate::ir::TypedArrayElement;
use crate::ir::value;

pub(super) fn scale(element: TypedArrayElement) -> Scale {
    match element {
        TypedArrayElement::Uint8 | TypedArrayElement::Uint8Clamped | TypedArrayElement::Int8 => Scale::One,
        TypedArrayElement::Uint16 | TypedArrayElement::Int16 => Scale::Two,
        TypedArrayElement::Int32 | TypedArrayElement::Uint32 | TypedArrayElement::Float32 => Scale::Four,
        TypedArrayElement::Float64 => Scale::Eight,
    }
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// The `TypedArrayBase::Kind` value of `element`.
    pub(super) fn typed_array_kind_value(&self, element: TypedArrayElement) -> u8 {
        let layout = &self.runtime.layout;
        match element {
            TypedArrayElement::Uint8 => layout.typed_array_kind_uint8,
            TypedArrayElement::Uint8Clamped => layout.typed_array_kind_uint8_clamped,
            TypedArrayElement::Int8 => layout.typed_array_kind_int8,
            TypedArrayElement::Uint16 => layout.typed_array_kind_uint16,
            TypedArrayElement::Int16 => layout.typed_array_kind_int16,
            TypedArrayElement::Int32 => layout.typed_array_kind_int32,
            TypedArrayElement::Uint32 => layout.typed_array_kind_uint32,
            TypedArrayElement::Float32 => layout.typed_array_kind_float32,
            TypedArrayElement::Float64 => layout.typed_array_kind_float64,
        }
    }

    /// Continues where `value` is an object whose elements are of `kind`,
    /// with its address in `object`, and branches to `fail` otherwise.
    fn branch_unless_elements_kind(&mut self, value: Gpr, object: Gpr, kind: ElementsKind, fail: Label) {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        self.branch_on_tag(Condition::NotEqual, value, value::OBJECT_TAG, scratch, fail);
        self.emit_unbox_cell(object, value);
        self.masm
            .load16(scratch, &Address::new(object, self.runtime.offsets.object_flags as i32));
        match kind {
            ElementsKind::TypedArray(element) => {
                self.masm.branch_test32(
                    Condition::Zero,
                    scratch,
                    u32::from(layout.object_flag_is_typed_array),
                    fail,
                );
                self.masm
                    .load8(scratch, &Address::new(object, layout.typed_array_kind as i32));
                let kind_value = self.typed_array_kind_value(element);
                self.masm
                    .branch32_imm(Condition::NotEqual, scratch, i32::from(kind_value), fail);
            }
            ElementsKind::Packed | ElementsKind::Holey => {
                self.masm.branch_test32(
                    Condition::NonZero,
                    scratch,
                    u32::from(layout.object_flag_is_typed_array | layout.object_flag_may_interfere),
                    fail,
                );
                self.masm.load8(
                    scratch,
                    &Address::new(object, layout.object_indexed_storage_kind as i32),
                );
                let packed = i32::from(layout.indexed_storage_kind_packed);
                if kind == ElementsKind::Packed {
                    self.masm.branch32_imm(Condition::NotEqual, scratch, packed, fail);
                } else {
                    let ok = self.masm.new_label();
                    self.masm.branch32_imm(Condition::Equal, scratch, packed, ok);
                    self.masm.branch32_imm(
                        Condition::NotEqual,
                        scratch,
                        i32::from(layout.indexed_storage_kind_holey),
                        fail,
                    );
                    self.masm.bind(ok);
                }
            }
        }
    }

    pub(super) fn emit_check_elements(&mut self, node: NodeId, kind: ElementsKind) {
        let exit = self.exit_site(node);
        let (value, object) = (self.input(node, 0), self.temp(node, 0));
        self.branch_unless_elements_kind(value, object, kind, exit);
    }

    /// A branch on `BranchCondition::ElementsKind`: to `target` where it
    /// holds, or where it does not, with `invert`.
    pub(super) fn emit_branch_on_elements_kind(
        &mut self,
        node: NodeId,
        kind: ElementsKind,
        target: Label,
        invert: bool,
    ) {
        let (value, object) = (self.input(node, 0), self.temp(node, 0));
        if invert {
            self.branch_unless_elements_kind(value, object, kind, target);
            return;
        }
        let other = self.masm.new_label();
        self.branch_unless_elements_kind(value, object, kind, other);
        self.masm.jump(target);
        self.masm.bind(other);
    }

    /// A `LoadTypedArrayLength` node.
    pub(super) fn emit_load_typed_array_length(&mut self, node: NodeId) {
        let (object, output) = (self.input(node, 0), self.output(node));
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        let done = self.masm.new_label();
        self.masm.move_imm32(output, 0);
        self.masm.load64(
            scratch,
            &Address::new(object, layout.typed_array_cached_data_offset as i32),
        );
        self.masm.branch64_imm(
            Condition::Equal,
            scratch,
            layout.typed_array_cached_data_offset_invalid as i64,
            done,
        );
        self.masm
            .load32(output, &Address::new(object, layout.typed_array_array_length as i32));
        self.masm.bind(done);
    }

    /// A `CheckBounds` node.
    pub(super) fn emit_check_bounds(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let (index, count) = (self.input(node, 0), self.input(node, 1));
        self.masm.branch32(Condition::AboveOrEqual, index, count, exit);
    }

    /// A `CheckNotHole` node.
    pub(super) fn emit_check_not_hole(&mut self, node: NodeId) {
        let exit = self.exit_site(node);
        let value = self.input(node, 0);
        self.masm
            .branch64_imm(Condition::Equal, value, value::EMPTY as i64, exit);
    }

    /// The address of element `index` of the object at `object`, whose
    /// elements are of `kind`. Uses `temp` and the scratch register.
    fn element_address(&mut self, object: Gpr, index: Gpr, temp: Gpr, kind: ElementsKind) -> Address {
        let scratch = self.pinned.scratch;
        let layout = self.runtime.layout;
        match kind {
            ElementsKind::Packed | ElementsKind::Holey => {
                self.masm
                    .load64(temp, &Address::new(object, layout.object_indexed_elements as i32));
                self.masm.move32(scratch, index);
                Address::indexed(temp, scratch, Scale::Eight, 0)
            }
            // `cage_base + ((data + index * size) & mask)`
            ElementsKind::TypedArray(element) => {
                self.masm.load64(
                    temp,
                    &Address::new(object, layout.typed_array_cached_data_offset as i32),
                );
                self.masm.move32(scratch, index);
                let shift = scale(element).log2();
                if shift != 0 {
                    self.masm.shl64_imm(scratch, scratch, shift);
                }
                self.masm.add64(temp, temp, scratch);
                self.masm
                    .and64_imm(temp, temp, layout.primitive_storage_cage_offset_mask);
                self.masm.load64(
                    scratch,
                    &Address::new(self.pinned.vm, layout.vm_primitive_storage_cage_base as i32),
                );
                self.masm.add64(temp, temp, scratch);
                Address::new(temp, 0)
            }
        }
    }

    /// A `LoadElementAt` node.
    pub(super) fn emit_load_element_at(&mut self, node: NodeId, kind: ElementsKind) {
        let (object, index) = (self.input(node, 0), self.input(node, 1));
        let temp = self.temp(node, 0);
        let address = self.element_address(object, index, temp, kind);
        let ElementsKind::TypedArray(element) = kind else {
            let output = self.output(node);
            self.masm.load64(output, &address);
            return;
        };
        if kind.loaded_repr() == Repr::Float64 {
            let output = self.float_output(node);
            match element {
                TypedArrayElement::Uint32 => {
                    self.masm.load32(temp, &address);
                    self.masm.convert_int64_to_double(output, temp);
                }
                TypedArrayElement::Float32 => self.masm.load_float_as_double(output, &address),
                _ => self.masm.load_double(output, &address),
            }
            return;
        }
        let output = self.output(node);
        match element {
            TypedArrayElement::Uint8 | TypedArrayElement::Uint8Clamped => self.masm.load8(output, &address),
            TypedArrayElement::Int8 => self.masm.load8_sign_extend(output, &address),
            TypedArrayElement::Uint16 => self.masm.load16(output, &address),
            TypedArrayElement::Int16 => self.masm.load16_sign_extend(output, &address),
            _ => self.masm.load32(output, &address),
        }
    }

    /// A `StoreElementAt` node.
    pub(super) fn emit_store_element_at(&mut self, node: NodeId, kind: ElementsKind) {
        let (object, index) = (self.input(node, 0), self.input(node, 1));
        let temp = self.temp(node, 0);
        let scratch = self.pinned.scratch;
        let address = self.element_address(object, index, temp, kind);
        if kind.stored_repr() == Repr::Float64 {
            let source = self.float_input(node, 2);
            if kind == ElementsKind::TypedArray(TypedArrayElement::Float32) {
                self.masm.store_double_as_float(&address, source);
            } else {
                self.masm.store_double(&address, source);
            }
            return;
        }
        let source = self.input(node, 2);
        let ElementsKind::TypedArray(element) = kind else {
            self.masm.store64(&address, source);
            return;
        };
        match element {
            TypedArrayElement::Uint8Clamped => {
                let (clamp_low, clamp_high, store) =
                    (self.masm.new_label(), self.masm.new_label(), self.masm.new_label());
                self.masm.move32(scratch, source);
                self.masm.branch32_imm(Condition::LessThan, scratch, 0, clamp_low);
                self.masm.branch32_imm(Condition::GreaterThan, scratch, 255, clamp_high);
                self.masm.jump(store);
                self.masm.bind(clamp_low);
                self.masm.move_imm32(scratch, 0);
                self.masm.jump(store);
                self.masm.bind(clamp_high);
                self.masm.move_imm32(scratch, 255);
                self.masm.bind(store);
                self.masm.store8(&address, scratch);
            }
            TypedArrayElement::Uint8 | TypedArrayElement::Int8 => self.masm.store8(&address, source),
            TypedArrayElement::Uint16 | TypedArrayElement::Int16 => self.masm.store16(&address, source),
            _ => self.masm.store32(&address, source),
        }
    }
}
