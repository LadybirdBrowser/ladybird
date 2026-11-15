/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The WebAssembly type language, and the canonicalization that decides when two type indices denote the same type.

use std::collections::HashMap;

/// A heap type. `Concrete` names a type by its index in the module's type section.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum HeapType {
    Func,
    NoFunc,
    Extern,
    NoExtern,
    Any,
    Eq,
    I31,
    Struct,
    Array,
    None,
    Exn,
    NoExn,
    Concrete(u32),
    /// The bottom of every hierarchy. Validation only produces it for values popped from an unreachable stack.
    Bottom,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RefType {
    pub nullable: bool,
    pub heap: HeapType,
}

impl RefType {
    pub const FUNCREF: RefType = RefType {
        nullable: true,
        heap: HeapType::Func,
    };
    pub const EXNREF: RefType = RefType {
        nullable: true,
        heap: HeapType::Exn,
    };

    pub fn new(nullable: bool, heap: HeapType) -> Self {
        Self { nullable, heap }
    }

    pub fn as_non_null(self) -> Self {
        Self {
            nullable: false,
            heap: self.heap,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ValType {
    I32,
    I64,
    F32,
    F64,
    V128,
    Ref(RefType),
}

impl ValType {
    pub const FUNCREF: ValType = ValType::Ref(RefType::FUNCREF);

    pub fn is_reference(self) -> bool {
        matches!(self, Self::Ref(_))
    }

    pub fn is_numeric(self) -> bool {
        matches!(self, Self::I32 | Self::I64 | Self::F32 | Self::F64)
    }

    /// Whether a local of this type has a default value, so that it can be used before it is set.
    pub fn is_defaultable(self) -> bool {
        match self {
            Self::Ref(reference) => reference.nullable,
            _ => true,
        }
    }

    /// The number of 8-byte slots a value of this type takes on the operand stack.
    pub fn slot_count(self) -> u32 {
        match self {
            Self::V128 => 2,
            _ => 1,
        }
    }

    /// This type with the type index of a concrete heap type replaced by `map` of it.
    pub fn map_type_indices(self, map: &impl Fn(u32) -> u32) -> Self {
        match self {
            Self::Ref(RefType {
                nullable,
                heap: HeapType::Concrete(index),
            }) => Self::Ref(RefType {
                nullable,
                heap: HeapType::Concrete(map(index)),
            }),
            other => other,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum StorageType {
    Val(ValType),
    I8,
    I16,
}

impl StorageType {
    /// The bytes a struct field or array element of this storage type takes in a GC object's data (see
    /// `abi::STORAGE_64`): its natural size, with references taking 8.
    pub fn object_field_size(self) -> u32 {
        match self {
            Self::I8 => 1,
            Self::I16 => 2,
            Self::Val(ValType::I32 | ValType::F32) => 4,
            Self::Val(ValType::V128) => 16,
            Self::Val(_) => 8,
        }
    }

    /// The alignment of a struct field of this storage type in a GC object's data, which is at most 8.
    pub fn object_field_alignment(self) -> u32 {
        self.object_field_size().min(8)
    }

    pub fn is_packed(self) -> bool {
        matches!(self, Self::I8 | Self::I16)
    }

    /// The type a field of this storage type has once it is read.
    pub fn unpacked(self) -> ValType {
        match self {
            Self::Val(value) => value,
            Self::I8 | Self::I16 => ValType::I32,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct FieldType {
    pub storage: StorageType,
    pub mutable: bool,
}

/// The byte offsets of the fields of a struct in a GC object's data, and the size of the data rounded up to 8 bytes.
/// Each field is aligned, and takes the first gap the fields before it left that it fits in, or else goes at the end.
/// A field's place depends only on the fields before it, so the fields a subtype shares with its supertype are where
/// they are in the supertype.
pub fn struct_layout(fields: &[FieldType]) -> (Vec<u32>, u32) {
    let mut offsets = Vec::with_capacity(fields.len());
    // The gaps alignment has left so far, as (start, end), in order.
    let mut gaps: Vec<(u32, u32)> = Vec::new();
    let mut end: u32 = 0;
    for field in fields {
        let size = field.storage.object_field_size();
        let alignment = field.storage.object_field_alignment();
        let fitting_gap = gaps.iter().enumerate().find_map(|(index, &(start, gap_end))| {
            let offset = start.next_multiple_of(alignment);
            (offset + size <= gap_end).then_some((index, offset))
        });
        let offset = match fitting_gap {
            Some((index, offset)) => {
                let (start, gap_end) = gaps[index];
                gaps.remove(index);
                if offset + size < gap_end {
                    gaps.insert(index, (offset + size, gap_end));
                }
                if start < offset {
                    gaps.insert(index, (start, offset));
                }
                offset
            }
            None => {
                let offset = end.next_multiple_of(alignment);
                if end < offset {
                    gaps.push((end, offset));
                }
                end = offset + size;
                offset
            }
        };
        offsets.push(offset);
    }
    (offsets, end.next_multiple_of(8))
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Default)]
pub struct FuncType {
    pub params: Vec<ValType>,
    pub results: Vec<ValType>,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum CompositeType {
    Func(FuncType),
    Struct(Vec<FieldType>),
    Array(FieldType),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct SubType {
    pub is_final: bool,
    pub supertypes: Vec<u32>,
    pub composite: CompositeType,
}

impl SubType {
    /// This type with every type index in it replaced by `map` of it.
    pub fn map_type_indices(&self, map: &impl Fn(u32) -> u32) -> Self {
        let storage = |storage: StorageType| match storage {
            StorageType::Val(value) => StorageType::Val(value.map_type_indices(map)),
            packed => packed,
        };
        let field = |field: &FieldType| FieldType {
            storage: storage(field.storage),
            mutable: field.mutable,
        };
        Self {
            is_final: self.is_final,
            supertypes: self.supertypes.iter().map(|&supertype| map(supertype)).collect(),
            composite: match &self.composite {
                CompositeType::Func(function) => CompositeType::Func(FuncType {
                    params: function
                        .params
                        .iter()
                        .map(|param| param.map_type_indices(map))
                        .collect(),
                    results: function
                        .results
                        .iter()
                        .map(|result| result.map_type_indices(map))
                        .collect(),
                }),
                CompositeType::Struct(fields) => CompositeType::Struct(fields.iter().map(field).collect()),
                CompositeType::Array(element) => CompositeType::Array(field(element)),
            },
        }
    }

    pub fn as_func(&self) -> Option<&FuncType> {
        match &self.composite {
            CompositeType::Func(function) => Some(function),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AddressType {
    I32,
    I64,
}

impl AddressType {
    pub fn value_type(self) -> ValType {
        match self {
            Self::I32 => ValType::I32,
            Self::I64 => ValType::I64,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Limits {
    pub min: u64,
    pub max: Option<u64>,
    pub address_type: AddressType,
    pub shared: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MemoryType {
    pub limits: Limits,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TableType {
    pub element: RefType,
    pub limits: Limits,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GlobalType {
    pub value: ValType,
    pub mutable: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TagType {
    pub type_index: u32,
}

/// A type index inside a canonicalization key: either relative to the start of the recursion group it appears in,
/// or the canonical identity of a type outside of it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
enum KeyIndex {
    Recursive(u32),
    Canonical(u32),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum KeyHeap {
    Abstract(HeapType),
    Index(KeyIndex),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum KeyValue {
    Plain(ValType),
    Ref(bool, KeyHeap),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum KeyStorage {
    Value(KeyValue),
    I8,
    I16,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
enum KeyComposite {
    Func(Vec<KeyValue>, Vec<KeyValue>),
    Struct(Vec<(KeyStorage, bool)>),
    Array(KeyStorage, bool),
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct KeySubType {
    is_final: bool,
    supertypes: Vec<KeyIndex>,
    composite: KeyComposite,
}

/// Assigns every type a canonical identity, such that two types are equal exactly when their identities are. Types are
/// equal when their recursion groups are structurally identical and they sit at the same position in them, so a whole
/// group is canonicalized at once, with references to types of earlier groups replaced by their identities.
#[derive(Default)]
pub struct TypeCanonicalizer {
    groups: HashMap<Vec<KeySubType>, u32>,
    next_identity: u32,
}

impl TypeCanonicalizer {
    pub fn new() -> Self {
        Self::default()
    }

    /// Canonicalize a recursion group whose first type has index `start`. `identity_of` gives the identities of the
    /// types before `start`. Returns the identity of each type in the group.
    pub fn canonicalize_group(&mut self, group: &[SubType], start: u32, identity_of: impl Fn(u32) -> u32) -> Vec<u32> {
        let index = |type_index: u32| {
            if type_index >= start {
                KeyIndex::Recursive(type_index - start)
            } else {
                KeyIndex::Canonical(identity_of(type_index))
            }
        };
        let value = |value: ValType| match value {
            ValType::Ref(reference) => KeyValue::Ref(
                reference.nullable,
                match reference.heap {
                    HeapType::Concrete(type_index) => KeyHeap::Index(index(type_index)),
                    heap => KeyHeap::Abstract(heap),
                },
            ),
            plain => KeyValue::Plain(plain),
        };
        let storage = |storage: StorageType| match storage {
            StorageType::Val(inner) => KeyStorage::Value(value(inner)),
            StorageType::I8 => KeyStorage::I8,
            StorageType::I16 => KeyStorage::I16,
        };
        let key = group
            .iter()
            .map(|sub_type| KeySubType {
                is_final: sub_type.is_final,
                supertypes: sub_type.supertypes.iter().map(|&supertype| index(supertype)).collect(),
                composite: match &sub_type.composite {
                    CompositeType::Func(function) => KeyComposite::Func(
                        function.params.iter().map(|&param| value(param)).collect(),
                        function.results.iter().map(|&result| value(result)).collect(),
                    ),
                    CompositeType::Struct(fields) => KeyComposite::Struct(
                        fields
                            .iter()
                            .map(|field| (storage(field.storage), field.mutable))
                            .collect(),
                    ),
                    CompositeType::Array(field) => KeyComposite::Array(storage(field.storage), field.mutable),
                },
            })
            .collect::<Vec<_>>();
        let count = group.len() as u32;
        let next_identity = &mut self.next_identity;
        let first = *self.groups.entry(key).or_insert_with(|| {
            let first = *next_identity;
            *next_identity += count;
            first
        });
        (first..first + count).collect()
    }
}
