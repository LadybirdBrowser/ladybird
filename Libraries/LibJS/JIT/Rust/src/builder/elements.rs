/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keyed accesses speculated on one elements kind, from the interpreter's
//! keyed feedback: a site that only ever used int32 indices in bounds of
//! packed or holey arrays (or of typed arrays of one kind) checks the kind of
//! the object, checks the index against the element count, and loads or
//! stores the element directly, checking that holey elements have no hole
//! there. Each check exits.

use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::ElementsKind;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::TypedArrayElement;
use crate::snapshot::keyed_feedback_bits::HOLEY;
use crate::snapshot::keyed_feedback_bits::INT32_INDEX;
use crate::snapshot::keyed_feedback_bits::KEY_KINDS_MASK;
use crate::snapshot::keyed_feedback_bits::OTHER_ELEMENTS;
use crate::snapshot::keyed_feedback_bits::OUT_OF_BOUNDS;
use crate::snapshot::keyed_feedback_bits::PACKED;
use crate::snapshot::keyed_feedback_bits::TYPED_ARRAY_SHIFT;

const PACKED_AND_HOLEY: u32 = PACKED | HOLEY;

impl GraphBuilder<'_> {
    /// The bits of the keyed feedback of `instruction`, if it has a slot.
    pub(super) fn keyed_feedback_bits(&self, instruction: &Instruction) -> Option<u32> {
        let slot = instruction.feedback_slots().keyed?;
        Some(self.function.executable.feedback.keyed.get(usize::from(slot))?.bits)
    }

    /// The typed array element kinds keyed feedback `bits` saw.
    pub(super) fn typed_array_elements_seen(&self, bits: u32) -> Vec<TypedArrayElement> {
        TypedArrayElement::ALL
            .into_iter()
            .filter(|element| bits & (1 << (TYPED_ARRAY_SHIFT + u32::from(self.typed_array_kind_value(*element)))) != 0)
            .collect()
    }

    /// The elements kind the keyed feedback of `instruction` saw, if it saw
    /// only int32 indices in bounds of one kind.
    fn speculated_elements_kind(&self, instruction: &Instruction) -> Option<ElementsKind> {
        let bits = self.keyed_feedback_bits(instruction)?;
        if bits & KEY_KINDS_MASK != INT32_INDEX || bits & (OTHER_ELEMENTS | OUT_OF_BOUNDS) != 0 {
            return None;
        }
        let typed_arrays = self.typed_array_elements_seen(bits);
        let kind = match (bits & (PACKED | HOLEY), typed_arrays.as_slice()) {
            (PACKED, []) => ElementsKind::Packed,
            // NB: Holey accesses take packed elements too.
            (HOLEY | PACKED_AND_HOLEY, []) => ElementsKind::Holey,
            (0, [element]) => ElementsKind::TypedArray(*element),
            _ => return None,
        };
        let gates = [ExitKind::BadElements, ExitKind::OutOfBounds, ExitKind::NotInt32];
        gates.into_iter().all(|kind| self.may_speculate(kind)).then_some(kind)
    }

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

    /// The storage of the elements of `kind` of the object at `address`,
    /// which element accesses take as their input 0, and the bound an index
    /// into them must be below. Packed and holey elements are accessed
    /// through the object, and must be below the array-like size and the
    /// capacity of their storage (which holey elements may have less of), so
    /// below the smaller of the two. Typed arrays are accessed through their
    /// storage offset, and must be below their length.
    ///
    /// Typed arrays known to have a fixed length use it as a constant once
    /// their storage offset is checked to be valid, which exits, so only
    /// where the elements kind may be speculated on. Accesses take the
    /// checked offset: with a constant index, the bounds check folds away,
    /// and the check of the offset is all that keeps them behind it.
    pub(super) fn element_storage_and_bound(&mut self, kind: ElementsKind, address: NodeId) -> (NodeId, NodeId) {
        let count = |builder: &mut Self, op| builder.emit(op, vec![address], Some(Repr::Int32));
        match kind {
            ElementsKind::Packed | ElementsKind::Holey => {
                let length = count(self, Op::LoadElementsLength);
                let capacity = count(self, Op::LoadElementsCapacity);
                let bound = self.emit(Op::Uint32Min, vec![length, capacity], Some(Repr::Int32));
                (address, bound)
            }
            ElementsKind::TypedArray(_) => {
                let fixed_length = self.graph.constant_value(address).and_then(|address| {
                    self.graph
                        .typed_array_constants
                        .iter()
                        .find(|(bits, _)| {
                            self.runtime.heap_region_base + (bits & self.runtime.heap_region_offset_mask) == address
                        })?
                        .1
                        .fixed_length
                });
                let offset =
                    |builder: &mut Self| builder.emit(Op::LoadTypedArrayDataOffset, vec![address], Some(Repr::Pointer));
                let data = |builder: &mut Self, offset| {
                    builder.emit(Op::TypedArrayDataAddress, vec![offset], Some(Repr::Pointer))
                };
                match fixed_length {
                    Some(length) if self.may_speculate(ExitKind::OutOfBounds) => {
                        let offset = offset(self);
                        let offset = self.emit_checked(Op::CheckTypedArrayStorage, vec![offset], Some(Repr::Pointer));
                        (data(self, offset), self.int32_constant(length as i32))
                    }
                    _ => {
                        let length = count(self, Op::LoadTypedArrayLength);
                        let offset = offset(self);
                        (data(self, offset), length)
                    }
                }
            }
        }
    }

    /// The element at `index` of the elements of `kind` with the `storage`
    /// that `element_storage_and_bound()` gave, with doubles boxed.
    pub(super) fn load_element(&mut self, kind: ElementsKind, storage: NodeId, index: NodeId) -> NodeId {
        let element = self.emit(
            Op::LoadElementAt { kind },
            vec![storage, index],
            Some(kind.loaded_repr()),
        );
        match kind.loaded_repr() {
            Repr::Float64 => self.emit(Op::BoxFloat64, vec![element], Some(Repr::Tagged)),
            _ => element,
        }
    }

    /// Stores `value` at the checked `index` of the elements of `kind` with
    /// the `storage` that `element_storage_and_bound()` gave.
    pub(super) fn store_element(&mut self, kind: ElementsKind, storage: NodeId, index: NodeId, value: NodeId) {
        self.emit(Op::StoreElementAt { kind }, vec![storage, index, value], None);
    }

    /// The storage of the checked object (see
    /// `element_storage_and_bound()`) and the checked index of an element
    /// access of `kind`: the object has elements of `kind`, and the index is
    /// in their bounds.
    fn element_access(
        &mut self,
        kind: ElementsKind,
        base: Operand,
        property: Operand,
    ) -> Result<(NodeId, NodeId), CompileFailure> {
        let object = self.read(base)?;
        let index = self.read_value(property)?;
        let index = self.int32(index);
        let object = self.emit_checked(Op::CheckElements { kind }, vec![object], None);
        let address = self.cell_address(object);
        let (storage, bound) = self.element_storage_and_bound(kind, address);
        let index = self.emit_checked(Op::CheckBounds, vec![index, bound], Some(Repr::Int32));
        Ok((storage, index))
    }

    /// Builds `dst = base[property]` as an element load, if the keyed
    /// feedback speculates on one elements kind. Returns false, having built
    /// nothing, otherwise.
    pub(super) fn try_build_element_load(
        &mut self,
        instruction: &Instruction,
        dst: Operand,
        base: Operand,
        property: Operand,
    ) -> Result<bool, CompileFailure> {
        let Some(kind) = self.speculated_elements_kind(instruction) else {
            return Ok(false);
        };
        let (storage, index) = self.element_access(kind, base, property)?;
        let mut value = self.load_element(kind, storage, index);
        if kind == ElementsKind::Holey {
            value = self.emit_checked(Op::CheckNotHole, vec![value], Some(Repr::Tagged));
        }
        self.write(dst, value)?;
        Ok(true)
    }

    /// Builds `base[property] = src` as an element store, if the keyed
    /// feedback speculates on one elements kind. Typed arrays check and
    /// convert numeric values. Returns false, having built nothing, otherwise.
    pub(super) fn try_build_element_store(
        &mut self,
        instruction: &Instruction,
        base: Operand,
        property: Operand,
        src: Operand,
    ) -> Result<bool, CompileFailure> {
        let Some(kind) = self.speculated_elements_kind(instruction) else {
            return Ok(false);
        };
        let value = self.read_value(src)?;
        let value = match (kind.stored_repr(), self.graph.node(value).repr) {
            (Repr::Int32, Some(Repr::Int32)) => value,
            (Repr::Float64, Some(Repr::Int32)) => self.emit(Op::Int32ToFloat64, vec![value], Some(Repr::Float64)),
            (Repr::Int32, _) if self.may_speculate(ExitKind::BadType) => {
                let number = self.number(value);
                let conversion = if kind == ElementsKind::TypedArray(TypedArrayElement::Uint8Clamped) {
                    Op::Float64ToUint8Clamped
                } else {
                    Op::Float64ToInt32
                };
                self.emit(conversion, vec![number], Some(Repr::Int32))
            }
            (Repr::Float64, _) if self.may_speculate(ExitKind::BadType) => self.number(value),
            (Repr::Tagged, _) => self.tagged(value),
            _ => return Ok(false),
        };
        let (storage, index) = self.element_access(kind, base, property)?;
        // NB: Holes are filled by the slow path.
        if kind == ElementsKind::Holey {
            let old = self.emit(Op::LoadElementAt { kind }, vec![storage, index], Some(Repr::Tagged));
            self.emit_checked(Op::CheckNotHole, vec![old], Some(Repr::Tagged));
        }
        self.store_element(kind, storage, index, value);
        Ok(true)
    }
}
