/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The GC instructions (the `0xFB` prefix): structs, arrays, i31 references, and tests and casts of references.
//!
//! Objects are cells of the runtime's heap (see `abi::Rtt`): the Rtt first, the data's address at OBJECT_DATA_OFFSET
//! and an array's length at OBJECT_LENGTH_OFFSET. Data of up to `MAX_INLINE_DATA_SIZE` bytes is inline at
//! OBJECT_INLINE_DATA_OFFSET, so a struct's fields are at constant offsets from the object where the struct is small
//! enough and its type is final. Fields take `StorageType::object_field_size()` bytes, with packed and 32-bit values
//! zero-extended in them: the runtime's layout, which `abi` and `types` define for both.
//!
//! GC references in compiled frames need nothing special: the collector scans the stack conservatively and recognizes
//! pointers into cells, and every allocation is a call of the runtime, around which values live in stack slots or in
//! registers the runtime saves. The data of a large object lives outside the heap, so its address does not keep the
//! object alive; that address is read where the access is (`ReadField` and `ElementAddress` are not pure), so the
//! object is live up to there.

use super::Builder;
use crate::ir::{Func, HeapTest, Helper, LoadKind, Node, Op, REF_TEST_CAST, REF_TEST_NULLABLE, Type, Value};
use libwasm_module::Module;
use libwasm_module::abi::{
    InstanceData, MAX_INLINE_DATA_SIZE, OBJECT_DATA_OFFSET, OBJECT_INLINE_DATA_OFFSET, OBJECT_LENGTH_OFFSET,
    TRAP_MEMORY_OUT_OF_BOUNDS, TRAP_NULL_REFERENCE, TRAP_TABLE_OUT_OF_BOUNDS,
};
use libwasm_module::opcode::gc::*;
use libwasm_module::opcode::simd::I64X2_EXTRACT_LANE;
use libwasm_module::types::{CompositeType, HeapType, StorageType, ValType};
use std::mem::offset_of;

/// The bytes an element of `storage` takes in a data segment.
fn data_bytes(storage: StorageType) -> u64 {
    match storage {
        StorageType::I8 => 1,
        StorageType::I16 => 2,
        StorageType::Val(ValType::I32 | ValType::F32) => 4,
        StorageType::Val(ValType::V128) => 16,
        StorageType::Val(_) => 8,
    }
}

/// The log2 of the bytes a store of a value of `storage` writes.
fn store_size_log2(storage: StorageType) -> u8 {
    match storage {
        StorageType::I8 => 0,
        StorageType::I16 => 1,
        StorageType::Val(ValType::I32 | ValType::F32) => 2,
        StorageType::Val(ValType::V128) => 4,
        StorageType::Val(_) => 3,
    }
}

fn struct_fields(module: &Module, type_index: u32) -> &[libwasm_module::types::FieldType] {
    match &module.types[type_index as usize].composite {
        CompositeType::Struct(fields) => fields,
        _ => unreachable!("validated code accesses fields of struct types"),
    }
}

fn array_storage(module: &Module, type_index: u32) -> StorageType {
    match &module.types[type_index as usize].composite {
        CompositeType::Array(element) => element.storage,
        _ => unreachable!("validated code accesses elements of array types"),
    }
}

/// The number of supertypes above the module's type `type_index`.
fn type_depth(module: &Module, mut type_index: u32) -> u32 {
    let mut depth = 0;
    while let Some(&supertype) = module.types[type_index as usize].supertypes.first() {
        type_index = supertype;
        depth += 1;
    }
    depth
}

impl Builder {
    /// The byte offset of field `field` in the data of a struct of the module's type `type_index`, or with `field`
    /// equal to the field count, the size of its data.
    fn field_offset(&mut self, module: &Module, type_index: u32, field: u32) -> u32 {
        let key = (u64::from(type_index) << 32) | u64::from(field);
        if let Some(offset) = self.field_offsets.get(key) {
            return offset;
        }
        let mut offset = 0;
        for (index, field_type) in struct_fields(module, type_index).iter().enumerate() {
            self.field_offsets
                .insert((u64::from(type_index) << 32) | index as u64, offset);
            offset += field_type.storage.object_field_size();
        }
        let count = struct_fields(module, type_index).len() as u64;
        self.field_offsets.insert((u64::from(type_index) << 32) | count, offset);
        self.field_offsets.get(key).expect("every field has an offset")
    }

    /// The runtime type of the module's type `type_index`.
    fn rtt(&mut self, func: &mut Func, type_index: u32) -> Value {
        self.pure(
            func,
            Node::new(Op::Rtt, Type::Pointer, 0, [self.instance, type_index, 0]),
        )
    }

    /// The size of the data of every struct that a reference to the module's struct type `type_index` can refer to.
    fn struct_data_size(&mut self, module: &Module, type_index: u32) -> Option<u32> {
        if !module.types[type_index as usize].is_final {
            return None;
        }
        let count = struct_fields(module, type_index).len() as u32;
        Some(self.field_offset(module, type_index, count))
    }

    /// Where the data of an object with `size` bytes of it is: the object and the offset of its inline data, or the
    /// address of its data.
    fn object_data(func: &mut Func, object: Value, size: Option<u32>) -> (Value, u32) {
        if size.is_some_and(|size| size as usize <= MAX_INLINE_DATA_SIZE) {
            return (object, OBJECT_INLINE_DATA_OFFSET as u32);
        }
        let data = Self::append(
            func,
            Node::new(Op::ReadField, Type::Pointer, 8, [object, OBJECT_DATA_OFFSET as u32, 0]),
        );
        (data, 0)
    }

    /// A field or element of `storage` at `base` + `offset`, sign-extended if it is packed and `signed`.
    fn load_field(&mut self, func: &mut Func, base: Value, offset: u32, storage: StorageType, signed: bool) -> Value {
        let (kind, ty) = match storage {
            StorageType::I8 if signed => (LoadKind::I32S8, Type::I32),
            StorageType::I8 => (LoadKind::I32U8, Type::I32),
            StorageType::I16 if signed => (LoadKind::I32S16, Type::I32),
            StorageType::I16 => (LoadKind::I32U16, Type::I32),
            StorageType::Val(ValType::I32) => (LoadKind::I32, Type::I32),
            StorageType::Val(ValType::F32) => (LoadKind::F32, Type::F32),
            StorageType::Val(ValType::F64) => (LoadKind::F64, Type::F64),
            StorageType::Val(ValType::I64) => (LoadKind::I64, Type::I64),
            StorageType::Val(ValType::Ref(_)) => (LoadKind::I64, Type::Ref),
            StorageType::Val(ValType::V128) => {
                let zero = self.constant(func, Type::I32, 0);
                // NB: The flags are the sub-opcode of `v128.load`, which is 0.
                return Self::append(func, Node::new(Op::VectorLoad, Type::V128, 0, [base, zero, offset]));
            }
        };
        self.load_at(func, kind, ty, base, offset)
    }

    fn check_null(func: &mut Func, reference: Value) {
        Self::append(
            func,
            Node::new(Op::CheckNull, Type::None, TRAP_NULL_REFERENCE as u8, [reference, 0, 0]),
        );
    }

    /// A call of the runtime that allocates an object of the module's type `type_index` with `size` bytes of zeroed
    /// data and, for an array, `length` elements (both i64 values).
    fn allocate(&mut self, func: &mut Func, type_index: u32, size: Value, length: Value) -> Value {
        let rtt = self.rtt(func, type_index);
        self.helper(func, Helper::Allocate, &[rtt, size, length], Type::Ref)
    }

    /// The bits of an element's value as the runtime's functions take them: the low and the high 64 bits.
    fn value_bits(&mut self, func: &mut Func, value: Value, storage: StorageType) -> (Value, Value) {
        let value = func.resolve(value);
        let zero = self.constant(func, Type::I64, 0);
        match storage {
            StorageType::Val(ValType::F32) => {
                let bits = self.pure(func, Node::new(Op::Reinterpret, Type::I32, 0, [value, 0, 0]));
                (bits, zero)
            }
            StorageType::Val(ValType::F64) => {
                let bits = self.pure(func, Node::new(Op::Reinterpret, Type::I64, 0, [value, 0, 0]));
                (bits, zero)
            }
            StorageType::Val(ValType::V128) => {
                let low = self.pure(
                    func,
                    Node::new(Op::ExtractLane, Type::I64, 0, [value, I64X2_EXTRACT_LANE, 0]),
                );
                let high = self.pure(
                    func,
                    Node::new(Op::ExtractLane, Type::I64, 0, [value, I64X2_EXTRACT_LANE, 1]),
                );
                (low, high)
            }
            _ => (value, zero),
        }
    }

    /// Whether `value` is the zero of its type, which every new object's fields start as.
    fn is_zero(&self, func: &Func, value: Value) -> bool {
        let value = func.resolve(value);
        func.node(value).constant() == Some(0) || (value == self.vector_zero && value != crate::ir::NONE)
    }

    /// The test of `reference` against `heap`, as an i32, or for a cast, a check that traps.
    fn ref_test(
        &mut self,
        func: &mut Func,
        module: &Module,
        reference: Value,
        heap: HeapType,
        nullable: bool,
        cast: bool,
    ) -> Value {
        let reference = func.resolve(reference);
        let (test, identity, depth) = match heap {
            HeapType::Any | HeapType::Func | HeapType::Extern | HeapType::Exn => (HeapTest::NonNull, None, 0),
            HeapType::Eq => (HeapTest::Eq, None, 0),
            HeapType::I31 => (HeapTest::I31, None, 0),
            HeapType::Struct => (HeapTest::Struct, None, 0),
            HeapType::Array => (HeapTest::Array, None, 0),
            HeapType::None | HeapType::NoFunc | HeapType::NoExtern | HeapType::NoExn | HeapType::Bottom => {
                (HeapTest::Nothing, None, 0)
            }
            HeapType::Concrete(type_index) => {
                let test = match module.types[type_index as usize].composite {
                    CompositeType::Func(_) => HeapTest::ConcreteFunction,
                    _ => HeapTest::ConcreteObject,
                };
                (
                    test,
                    Some(self.type_identity(func, type_index)),
                    type_depth(module, type_index),
                )
            }
        };
        let identity = match identity {
            Some(identity) => identity,
            None => self.constant(func, Type::I32, 0),
        };
        let mut flags = test as u8;
        if nullable {
            flags |= REF_TEST_NULLABLE;
        }
        if cast {
            flags |= REF_TEST_CAST;
            return Self::append(
                func,
                Node::new(Op::RefTest, Type::None, flags, [reference, identity, depth]),
            );
        }
        self.pure(
            func,
            Node::new(Op::RefTest, Type::I32, flags, [reference, identity, depth]),
        )
    }

    /// The instructions of the `0xFB` prefix, with the immediates the decoder kept.
    pub(super) fn gc(
        &mut self,
        func: &mut Func,
        module: &Module,
        sub_opcode: u32,
        [first, second]: [u32; 2],
        heap_types: [HeapType; 2],
        cast_flags: u8,
    ) {
        match sub_opcode {
            STRUCT_NEW | STRUCT_NEW_DEFAULT => {
                let count = struct_fields(module, first).len();
                let size = self.field_offset(module, first, count as u32);
                let mut values = Vec::new();
                if sub_opcode == STRUCT_NEW {
                    values = self.stack.split_off(self.stack.len() - count);
                }
                let size_value = self.constant(func, Type::I64, u64::from(size));
                let zero = self.constant(func, Type::I64, 0);
                let object = self.allocate(func, first, size_value, zero);
                if !values.is_empty() {
                    let (base, data_offset) = Self::object_data(func, object, Some(size));
                    for (index, &value) in values.iter().enumerate() {
                        if self.is_zero(func, value) {
                            continue;
                        }
                        let storage = struct_fields(module, first)[index].storage;
                        let offset = data_offset + self.field_offset(module, first, index as u32);
                        self.store_at(func, store_size_log2(storage), base, offset, value);
                    }
                }
                self.push(object);
            }
            STRUCT_GET | STRUCT_GET_S | STRUCT_GET_U => {
                let object = func.resolve(self.pop());
                Self::check_null(func, object);
                let size = self.struct_data_size(module, first);
                let (base, data_offset) = Self::object_data(func, object, size);
                let offset = data_offset + self.field_offset(module, first, second);
                let storage = struct_fields(module, first)[second as usize].storage;
                let value = self.load_field(func, base, offset, storage, sub_opcode == STRUCT_GET_S);
                self.push(value);
            }
            STRUCT_SET => {
                let value = self.pop();
                let object = func.resolve(self.pop());
                Self::check_null(func, object);
                let size = self.struct_data_size(module, first);
                let (base, data_offset) = Self::object_data(func, object, size);
                let offset = data_offset + self.field_offset(module, first, second);
                let storage = struct_fields(module, first)[second as usize].storage;
                self.store_at(func, store_size_log2(storage), base, offset, value);
            }
            ARRAY_NEW | ARRAY_NEW_DEFAULT => {
                let count = self.pop();
                let storage = array_storage(module, first);
                let length = self.unary(func, Op::ExtendI32U, Type::I64, count);
                let element_size = self.constant(func, Type::I64, u64::from(storage.object_field_size()));
                let size = self.binary(func, Op::Mul, Type::I64, length, element_size);
                let array = if sub_opcode == ARRAY_NEW {
                    let value = self.pop();
                    let array = self.allocate(func, first, size, length);
                    if !self.is_zero(func, value) {
                        let (low, high) = self.value_bits(func, value, storage);
                        let zero = self.constant(func, Type::I64, 0);
                        self.helper(func, Helper::ArrayFill, &[array, zero, low, high, length], Type::None);
                    }
                    array
                } else {
                    self.allocate(func, first, size, length)
                };
                self.push(array);
            }
            ARRAY_NEW_FIXED => {
                let storage = array_storage(module, first);
                let values = self.stack.split_off(self.stack.len() - second as usize);
                let element_size = storage.object_field_size();
                let data_size = second * element_size;
                let size = self.constant(func, Type::I64, u64::from(data_size));
                let length = self.constant(func, Type::I64, u64::from(second));
                let array = self.allocate(func, first, size, length);
                let (base, data_offset) = Self::object_data(func, array, Some(data_size));
                for (index, &value) in values.iter().enumerate() {
                    if self.is_zero(func, value) {
                        continue;
                    }
                    let offset = data_offset + index as u32 * element_size;
                    self.store_at(func, store_size_log2(storage), base, offset, value);
                }
                self.push(array);
            }
            ARRAY_NEW_DATA | ARRAY_NEW_ELEM => {
                let count = self.pop();
                let source = self.pop();
                let storage = array_storage(module, first);
                let (segments, unit, trap, init) = if sub_opcode == ARRAY_NEW_DATA {
                    (
                        offset_of!(InstanceData, data_segments),
                        data_bytes(storage),
                        TRAP_MEMORY_OUT_OF_BOUNDS,
                        Helper::ArrayInitData,
                    )
                } else {
                    (
                        offset_of!(InstanceData, element_segments),
                        1,
                        TRAP_TABLE_OUT_OF_BOUNDS,
                        Helper::ArrayInitElem,
                    )
                };
                let segment = self.segment(func, segments, second);
                // The segment's range is checked first, before the array is allocated.
                let unit = self.constant(func, Type::I64, unit);
                let trap = self.constant(func, Type::I64, u64::from(trap));
                self.helper(
                    func,
                    Helper::SegmentRange,
                    &[segment, source, count, unit, trap],
                    Type::None,
                );
                let length = self.unary(func, Op::ExtendI32U, Type::I64, count);
                let element_size = self.constant(func, Type::I64, u64::from(storage.object_field_size()));
                let size = self.binary(func, Op::Mul, Type::I64, length, element_size);
                let array = self.allocate(func, first, size, length);
                let zero = self.constant(func, Type::I64, 0);
                self.helper(func, init, &[array, segment, zero, source, count], Type::None);
                self.push(array);
            }
            ARRAY_GET | ARRAY_GET_S | ARRAY_GET_U => {
                let index = func.resolve(self.pop());
                let array = func.resolve(self.pop());
                let storage = array_storage(module, first);
                let size_log2 = storage.object_field_size().trailing_zeros() as u8;
                let address = Self::append(
                    func,
                    Node::new(Op::ElementAddress, Type::Pointer, size_log2, [array, index, 0]),
                );
                let value = self.load_field(func, address, 0, storage, sub_opcode == ARRAY_GET_S);
                self.push(value);
            }
            ARRAY_SET => {
                let value = self.pop();
                let index = func.resolve(self.pop());
                let array = func.resolve(self.pop());
                let storage = array_storage(module, first);
                let size_log2 = storage.object_field_size().trailing_zeros() as u8;
                let address = Self::append(
                    func,
                    Node::new(Op::ElementAddress, Type::Pointer, size_log2, [array, index, 0]),
                );
                self.store_at(func, store_size_log2(storage), address, 0, value);
            }
            ARRAY_LEN => {
                let array = func.resolve(self.pop());
                Self::check_null(func, array);
                let length = Self::append(
                    func,
                    Node::new(Op::ReadField, Type::I32, 4, [array, OBJECT_LENGTH_OFFSET as u32, 0]),
                );
                self.push(length);
            }
            ARRAY_FILL => {
                let count = self.pop();
                let value = self.pop();
                let start = self.pop();
                let array = self.pop();
                let (low, high) = self.value_bits(func, value, array_storage(module, first));
                self.helper(func, Helper::ArrayFill, &[array, start, low, high, count], Type::None);
            }
            ARRAY_COPY => {
                let count = self.pop();
                let source_start = self.pop();
                let source = self.pop();
                let destination_start = self.pop();
                let destination = self.pop();
                self.helper(
                    func,
                    Helper::ArrayCopy,
                    &[destination, destination_start, source, source_start, count],
                    Type::None,
                );
            }
            ARRAY_INIT_DATA | ARRAY_INIT_ELEM => {
                let count = self.pop();
                let source = self.pop();
                let destination = self.pop();
                let array = self.pop();
                let (segments, init) = if sub_opcode == ARRAY_INIT_DATA {
                    (offset_of!(InstanceData, data_segments), Helper::ArrayInitData)
                } else {
                    (offset_of!(InstanceData, element_segments), Helper::ArrayInitElem)
                };
                let segment = self.segment(func, segments, second);
                self.helper(func, init, &[array, segment, destination, source, count], Type::None);
            }
            REF_TEST | REF_TEST_NULL => {
                let reference = self.pop();
                let value = self.ref_test(
                    func,
                    module,
                    reference,
                    heap_types[0],
                    sub_opcode == REF_TEST_NULL,
                    false,
                );
                self.push(value);
            }
            REF_CAST | REF_CAST_NULL => {
                let reference = func.resolve(self.pop());
                self.ref_test(
                    func,
                    module,
                    reference,
                    heap_types[0],
                    sub_opcode == REF_CAST_NULL,
                    true,
                );
                self.push(reference);
            }
            BR_ON_CAST | BR_ON_CAST_FAIL => {
                let reference = *self.stack.last().expect("br_on_cast has an operand");
                let nullable = cast_flags & 2 != 0;
                let matches = self.ref_test(func, module, reference, heap_types[1], nullable, false);
                let condition = if sub_opcode == BR_ON_CAST {
                    matches
                } else {
                    self.unary(func, Op::Eqz, Type::I32, matches)
                };
                self.conditional_branch(func, first, condition);
            }
            ANY_CONVERT_EXTERN | EXTERN_CONVERT_ANY => {
                // References keep their bits between the any and extern hierarchies.
            }
            REF_I31 => {
                let value = self.pop();
                let reference = self.unary(func, Op::I31New, Type::Ref, value);
                self.push(reference);
            }
            I31_GET_S | I31_GET_U => {
                let reference = func.resolve(self.pop());
                Self::check_null(func, reference);
                let flags = u8::from(sub_opcode == I31_GET_S);
                let value = self.pure(func, Node::new(Op::I31Get, Type::I32, flags, [reference, 0, 0]));
                self.push(value);
            }
            _ => unreachable!("validated code has no unknown 0xFB instruction {sub_opcode:#x}"),
        }
    }
}
