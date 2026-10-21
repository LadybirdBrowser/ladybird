/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime: the store of functions, memories, tables, and globals that instances share, instantiation, and
//! calling into the interpreter.

pub mod constant;
pub mod gc;
pub mod helpers;
pub mod interpreter;
pub mod memory;

use crate::abi::{
    EntryRecord, ExecutionContext, FRAME_HEADER_SIZE, FUNCTION_KIND_HOST, FUNCTION_KIND_WASM,
    FUNCTION_KIND_WASM_VECTOR, FunctionHeader, InstanceData, MemoryData, SPILL_AREA_SIZE, Segment, TableData,
};
use crate::module::{DataMode, ElementItems, ElementMode, ExternalKind, ImportDescription, Module};
use crate::types::{FuncType, GlobalType, HeapType, TableType, TypeCanonicalizer, ValType};
use memory::MemoryInstance;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;

/// A value as the runtime's API sees it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Value {
    I32(i32),
    I64(i64),
    /// The bits of an f32.
    F32(u32),
    /// The bits of an f64.
    F64(u64),
    V128(u128),
    /// A reference: zero for null, otherwise its encoded bits.
    Ref(u64),
}

impl Value {
    pub fn default_for(ty: ValType) -> Self {
        match ty {
            ValType::I32 => Self::I32(0),
            ValType::I64 => Self::I64(0),
            ValType::F32 => Self::F32(0),
            ValType::F64 => Self::F64(0),
            ValType::V128 => Self::V128(0),
            ValType::Ref(_) => Self::Ref(0),
        }
    }

    /// Append the slots of this value in memory order: a v128's low half first.
    pub fn push_slots(self, slots: &mut Vec<u64>) {
        match self {
            Self::I32(value) => slots.push(u64::from(value as u32)),
            Self::I64(value) => slots.push(value as u64),
            Self::F32(bits) => slots.push(u64::from(bits)),
            Self::F64(bits) | Self::Ref(bits) => slots.push(bits),
            Self::V128(bits) => {
                slots.push(bits as u64);
                slots.push((bits >> 64) as u64);
            }
        }
    }

    pub fn from_slots(ty: ValType, slots: &[u64]) -> Self {
        match ty {
            ValType::I32 => Self::I32(slots[0] as u32 as i32),
            ValType::I64 => Self::I64(slots[0] as i64),
            ValType::F32 => Self::F32(slots[0] as u32),
            ValType::F64 => Self::F64(slots[0]),
            ValType::Ref(_) => Self::Ref(slots[0]),
            ValType::V128 => Self::V128(u128::from(slots[0]) | (u128::from(slots[1]) << 64)),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Trap {
    Unreachable,
    StackExhausted,
    MemoryOutOfBounds,
    IntegerDivideByZero,
    IntegerOverflow,
    InvalidConversion,
    UndefinedElement,
    UninitializedElement,
    IndirectCallTypeMismatch,
    TableOutOfBounds,
    NullReference,
    CastFailure,
    ArrayOutOfBounds,
    UnalignedAtomic,
    ExpectedSharedMemory,
    Host(String),
    UncaughtException(u64),
}

impl Trap {
    fn from_code(code: u32, pending: Option<Trap>) -> Self {
        use crate::abi::*;
        match code {
            TRAP_UNREACHABLE => Self::Unreachable,
            TRAP_STACK_EXHAUSTED => Self::StackExhausted,
            TRAP_MEMORY_OUT_OF_BOUNDS => Self::MemoryOutOfBounds,
            TRAP_INTEGER_DIVIDE_BY_ZERO => Self::IntegerDivideByZero,
            TRAP_INTEGER_OVERFLOW => Self::IntegerOverflow,
            TRAP_INVALID_CONVERSION => Self::InvalidConversion,
            TRAP_UNDEFINED_ELEMENT => Self::UndefinedElement,
            TRAP_UNINITIALIZED_ELEMENT => Self::UninitializedElement,
            TRAP_INDIRECT_CALL_TYPE_MISMATCH => Self::IndirectCallTypeMismatch,
            TRAP_TABLE_OUT_OF_BOUNDS => Self::TableOutOfBounds,
            TRAP_NULL_REFERENCE => Self::NullReference,
            TRAP_CAST_FAILURE => Self::CastFailure,
            TRAP_ARRAY_OUT_OF_BOUNDS => Self::ArrayOutOfBounds,
            TRAP_UNALIGNED_ATOMIC => Self::UnalignedAtomic,
            TRAP_EXPECTED_SHARED_MEMORY => Self::ExpectedSharedMemory,
            _ => pending.unwrap_or_else(|| Self::Host("the runtime reported a trap without details".to_string())),
        }
    }

    pub fn message(&self) -> String {
        match self {
            Self::Unreachable => "unreachable".to_string(),
            Self::StackExhausted => "call stack exhausted".to_string(),
            Self::MemoryOutOfBounds => "Memory access out of bounds".to_string(),
            Self::IntegerDivideByZero => "integer divide by zero".to_string(),
            Self::IntegerOverflow => "integer overflow".to_string(),
            Self::InvalidConversion => "invalid conversion to integer".to_string(),
            Self::UndefinedElement => "undefined element".to_string(),
            Self::UninitializedElement => "uninitialized element".to_string(),
            Self::IndirectCallTypeMismatch => "indirect call type mismatch".to_string(),
            Self::TableOutOfBounds => "out of bounds table access".to_string(),
            Self::NullReference => "null reference".to_string(),
            Self::CastFailure => "cast failure".to_string(),
            Self::ArrayOutOfBounds => "out of bounds array access".to_string(),
            Self::Host(message) => message.clone(),
            Self::UncaughtException(_) => "uncaught exception".to_string(),
            Self::UnalignedAtomic => "Unaligned atomic memory access".to_string(),
            Self::ExpectedSharedMemory => "Expected shared memory".to_string(),
        }
    }
}

macro_rules! define_address {
    ($name:ident) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub struct $name(pub u32);
    };
}
define_address!(FunctionAddress);
define_address!(TableAddress);
define_address!(MemoryAddress);
define_address!(GlobalAddress);
define_address!(InstanceAddress);
define_address!(TagAddress);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExternValue {
    Function(FunctionAddress),
    Table(TableAddress),
    Memory(MemoryAddress),
    Global(GlobalAddress),
    Tag(TagAddress),
}

/// A tag, which exceptions are thrown with. Its address is its identity.
pub struct TagInstance {
    pub address: TagAddress,
    /// The types of the values an exception with this tag carries.
    pub ty: FuncType,
    pub type_identity: u32,
}

/// An exception: a tag and the values thrown with it, in operand stack order (the last value in the lowest slot).
/// An exception reference is the address of its ExceptionInstance.
pub struct ExceptionInstance {
    pub tag: TagAddress,
    pub payload: Box<[u64]>,
}

/// A function the embedder implements. It gets the arguments and returns the results, in the function type's order.
pub type HostCallback = Rc<dyn Fn(&mut Store, &[Value]) -> Result<Vec<Value>, Trap>>;

pub enum FunctionKind {
    Wasm {
        module: Arc<Module>,
        index: u32,
        instance: InstanceAddress,
    },
    Host(HostCallback),
}

/// A function. The interpreter sees its header, which is its first field, so that a function reference, which points
/// at the header, also points at the function.
#[repr(C)]
pub struct FunctionInstance {
    pub header: FunctionHeader,
    pub address: FunctionAddress,
    pub ty: FuncType,
    /// What `header.vector_storage` points at.
    pub vector_storage: Box<[u32]>,
    pub kind: FunctionKind,
}

pub struct TableInstance {
    pub data: Box<TableData>,
    pub ty: TableType,
    pub elements: Vec<u64>,
}

impl TableInstance {
    fn new(ty: TableType, initial: u64) -> Self {
        let mut elements = vec![initial; ty.limits.min as usize];
        let data = Box::new(TableData {
            elements: elements.as_mut_ptr(),
            size: elements.len() as u64,
            address64: u64::from(ty.limits.address_type == crate::types::AddressType::I64),
        });
        Self { data, ty, elements }
    }

    fn sync(&mut self) {
        self.data.elements = self.elements.as_mut_ptr();
        self.data.size = self.elements.len() as u64;
    }

    pub fn grow(&mut self, delta: u64, initial: u64) -> Option<u64> {
        let previous = self.elements.len() as u64;
        let new_size = previous.checked_add(delta)?;
        let maximum = self.ty.limits.max.unwrap_or(u64::from(u32::MAX));
        if new_size > maximum || new_size > MAX_TABLE_SIZE {
            return None;
        }
        self.elements.resize(new_size as usize, initial);
        self.sync();
        Some(previous)
    }
}

/// The largest number of elements a table may grow to.
const MAX_TABLE_SIZE: u64 = 10_000_000;

#[repr(C, align(16))]
pub struct GlobalInstance {
    /// The value's slots: one, or two for v128.
    pub value: [u64; 2],
    pub ty: GlobalType,
}

impl GlobalInstance {
    pub fn get(&self) -> Value {
        Value::from_slots(self.ty.value, &self.value)
    }

    pub fn set(&mut self, value: Value) {
        let mut slots = Vec::with_capacity(2);
        value.push_slots(&mut slots);
        self.value[..slots.len()].copy_from_slice(&slots);
    }
}

/// An instance. The interpreter sees its `data`.
pub struct Instance {
    pub data: Box<InstanceData>,
    pub module: Arc<Module>,
    pub functions: Vec<FunctionAddress>,
    pub tables: Vec<TableAddress>,
    pub memories: Vec<MemoryAddress>,
    pub globals: Vec<GlobalAddress>,
    pub tags: Vec<TagAddress>,
    pub exports: HashMap<String, ExternValue>,
    pub type_identities: Vec<u32>,
    /// The references of each passive element segment. The interpreter sees them through `element_segments`.
    pub element_storage: Vec<Vec<u64>>,
    /// Each element segment, empty once dropped or used by instantiation.
    pub element_segments: Vec<Segment>,
    /// Each data segment, empty once dropped or used by instantiation.
    pub data_segments: Vec<Segment>,
    function_headers: Vec<*const FunctionHeader>,
    global_slots: Vec<*mut u64>,
    table_datas: Vec<*mut TableData>,
    memory_datas: Vec<*mut MemoryData>,
    tag_identities: Vec<u64>,
}

impl Instance {
    pub fn export(&self, name: &str) -> Option<ExternValue> {
        self.exports.get(name).copied()
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum InstantiationError {
    Link(String),
    Trap(Trap),
}

/// The bytes of stack a call into WebAssembly leaves free for the runtime functions it calls.
const STACK_RESERVE: usize = 256 * 1024;

pub struct Store {
    context: Box<ExecutionContext>,
    pub functions: Vec<Box<FunctionInstance>>,
    pub tables: Vec<Box<TableInstance>>,
    pub memories: Vec<Box<MemoryInstance>>,
    pub globals: Vec<Box<GlobalInstance>>,
    pub instances: Vec<Box<Instance>>,
    pub tags: Vec<Box<TagInstance>>,
    /// The runtime type of each type identity, and the addresses the interpreter sees them by.
    #[allow(clippy::vec_box)] // The interpreter holds the addresses of the Rtts, which must not move.
    rtts: Vec<Box<gc::RttStorage>>,
    rtt_pointers: Vec<*const crate::abi::Rtt>,
    // FIXME: Collect objects nothing refers to anymore.
    objects: Vec<Box<[u64]>>,
    // FIXME: Collect exceptions nothing refers to anymore.
    pub exceptions: Vec<Box<ExceptionInstance>>,
    canonicalizer: TypeCanonicalizer,
    /// The type of each store-wide type identity, with its type indices replaced by identities, so that types of
    /// different modules can be compared.
    types: Vec<crate::types::SubType>,
    /// The identity of each type in `types`: its index. This makes `types` usable as a type context.
    identities: Vec<u32>,
    /// A trap a runtime function reported, which the interpreter only knows as TRAP_RUNTIME.
    pending_trap: Option<Trap>,
}

impl Default for Store {
    fn default() -> Self {
        Self::new()
    }
}

impl Store {
    pub fn new() -> Self {
        interpreter::check_cpu_features();
        interpreter::install_fault_handler();
        Self {
            context: Box::new(ExecutionContext {
                stack_limit: 0,
                entry_fp: 0,
                entry_record: std::ptr::null_mut(),
                store: std::ptr::null_mut(),
                arguments: [0; 6],
                rtts: std::ptr::null(),
            }),
            functions: Vec::new(),
            tables: Vec::new(),
            memories: Vec::new(),
            globals: Vec::new(),
            instances: Vec::new(),
            canonicalizer: TypeCanonicalizer::new(),
            types: Vec::new(),
            identities: Vec::new(),
            pending_trap: None,
            tags: Vec::new(),
            exceptions: Vec::new(),
            rtts: Vec::new(),
            rtt_pointers: Vec::new(),
            objects: Vec::new(),
        }
    }

    pub fn function(&self, address: FunctionAddress) -> &FunctionInstance {
        &self.functions[address.0 as usize]
    }

    pub fn memory(&self, address: MemoryAddress) -> &MemoryInstance {
        &self.memories[address.0 as usize]
    }

    pub fn memory_mut(&mut self, address: MemoryAddress) -> &mut MemoryInstance {
        &mut self.memories[address.0 as usize]
    }

    pub fn table(&self, address: TableAddress) -> &TableInstance {
        &self.tables[address.0 as usize]
    }

    pub fn table_mut(&mut self, address: TableAddress) -> &mut TableInstance {
        &mut self.tables[address.0 as usize]
    }

    pub fn global(&self, address: GlobalAddress) -> &GlobalInstance {
        &self.globals[address.0 as usize]
    }

    pub fn global_mut(&mut self, address: GlobalAddress) -> &mut GlobalInstance {
        &mut self.globals[address.0 as usize]
    }

    pub fn instance(&self, address: InstanceAddress) -> &Instance {
        &self.instances[address.0 as usize]
    }

    /// The store-wide identity of a function type that is not part of a recursion group.
    fn identity_of_function_type(&mut self, ty: &FuncType) -> u32 {
        let sub_type = crate::types::SubType {
            is_final: true,
            supertypes: Vec::new(),
            composite: crate::types::CompositeType::Func(ty.clone()),
        };
        let identities = self
            .canonicalizer
            .canonicalize_group(std::slice::from_ref(&sub_type), 0, |_| 0);
        self.register_types(&[sub_type], &identities);
        identities[0]
    }

    /// Remember the store-wide form of a canonicalized group the first time it is seen. Its types must already have
    /// identities in place of their type indices.
    fn register_types(&mut self, group: &[crate::types::SubType], identities: &[u32]) {
        if identities
            .first()
            .is_none_or(|&first| first as usize != self.types.len())
        {
            return;
        }
        for sub_type in group {
            let identity = self.types.len() as u32;
            self.identities.push(identity);
            self.types.push(sub_type.clone());
            let rtt = gc::RttStorage::new(self, identity);
            self.rtt_pointers.push(&raw const rtt.rtt);
            self.rtts.push(rtt);
        }
        self.context.rtts = self.rtt_pointers.as_ptr();
    }

    /// The types of the store, where concrete heap types are identities.
    fn type_context(&self) -> crate::validator::TypeContext<'_> {
        crate::validator::TypeContext {
            types: &self.types,
            identities: &self.identities,
        }
    }

    pub fn allocate_host_function(&mut self, ty: FuncType, callback: HostCallback) -> FunctionAddress {
        let address = FunctionAddress(self.functions.len() as u32);
        let type_identity = self.identity_of_function_type(&ty);
        let mut function = Box::new(FunctionInstance {
            header: FunctionHeader {
                code: std::ptr::null(),
                code_end: std::ptr::null(),
                side_table: std::ptr::null(),
                instance: std::ptr::null(),
                kind: FUNCTION_KIND_HOST,
                param_slots: ty.params.iter().map(|param| param.slot_count()).sum(),
                result_slots: ty.results.iter().map(|result| result.slot_count()).sum(),
                local_count: 0,
                locals_size: 0,
                reserved: 0,
                frame_size: 0,
                type_identity,
                host: std::ptr::null(),
                vector_storage: std::ptr::null(),
            },
            address,
            ty,
            vector_storage: Box::new([]),
            kind: FunctionKind::Host(callback),
        });
        function.header.host = (&raw const *function).cast();
        self.functions.push(function);
        address
    }

    pub fn allocate_memory(&mut self, ty: crate::types::MemoryType) -> Option<MemoryAddress> {
        let memory = MemoryInstance::new(ty)?;
        self.memories.push(Box::new(memory));
        Some(MemoryAddress(self.memories.len() as u32 - 1))
    }

    pub fn allocate_table(&mut self, ty: TableType, initial: u64) -> TableAddress {
        self.tables.push(Box::new(TableInstance::new(ty, initial)));
        TableAddress(self.tables.len() as u32 - 1)
    }

    pub fn allocate_global(&mut self, ty: GlobalType, value: Value) -> GlobalAddress {
        let mut global = Box::new(GlobalInstance { value: [0; 2], ty });
        global.set(value);
        self.globals.push(global);
        GlobalAddress(self.globals.len() as u32 - 1)
    }

    pub fn instantiate(
        &mut self,
        module: Arc<Module>,
        imports: &[ExternValue],
    ) -> Result<InstanceAddress, InstantiationError> {
        let link_error = |message: &str| Err(InstantiationError::Link(message.to_string()));
        if imports.len() != module.imports.len() {
            return link_error("wrong number of imports");
        }

        // Give the module's types store-wide identities.
        let mut type_identities: Vec<u32> = Vec::with_capacity(module.types.len());
        for group in &module.recursion_groups {
            let types = &module.types[group.start as usize..(group.start + group.count) as usize];
            let identities = self
                .canonicalizer
                .canonicalize_group(types, group.start, |index| type_identities[index as usize]);
            let map = |index: u32| {
                if index >= group.start {
                    identities[(index - group.start) as usize]
                } else {
                    type_identities[index as usize]
                }
            };
            let store_types: Vec<_> = types.iter().map(|ty| ty.map_type_indices(&map)).collect();
            self.register_types(&store_types, &identities);
            type_identities.extend(identities);
        }

        // Types of the store have identities where the module's types have indices.
        let module_identities = type_identities.clone();
        let canonical = |value: ValType| value.map_type_indices(&|index| module_identities[index as usize]);
        let mut functions = Vec::new();
        let mut tables = Vec::new();
        let mut memories = Vec::new();
        let mut globals = Vec::new();
        let mut tags = Vec::new();
        for (import, value) in module.imports.iter().zip(imports) {
            match (&import.description, value) {
                (ImportDescription::Function(type_index), ExternValue::Function(address)) => {
                    let expected = type_identities[*type_index as usize];
                    let actual = self.function(*address).header.type_identity;
                    if !self
                        .type_context()
                        .heap_subtype(HeapType::Concrete(actual), HeapType::Concrete(expected))
                    {
                        return link_error("incompatible import type");
                    }
                    functions.push(*address);
                }
                (ImportDescription::Table(ty), ExternValue::Table(address)) => {
                    let table = self.table(*address);
                    if !limits_match(table.elements.len() as u64, table.ty.limits.max, &ty.limits)
                        || table.ty.limits.address_type != ty.limits.address_type
                        || ValType::Ref(table.ty.element) != canonical(ValType::Ref(ty.element))
                    {
                        return link_error("incompatible import type");
                    }
                    tables.push(*address);
                }
                (ImportDescription::Memory(ty), ExternValue::Memory(address)) => {
                    let memory = self.memory(*address);
                    if !limits_match(memory.pages(), memory.ty.limits.max, &ty.limits)
                        || memory.ty.limits.address_type != ty.limits.address_type
                        || memory.ty.limits.shared != ty.limits.shared
                    {
                        return link_error("incompatible import type");
                    }
                    memories.push(*address);
                }
                (ImportDescription::Global(ty), ExternValue::Global(address)) => {
                    let global = self.global(*address);
                    let expected = canonical(ty.value);
                    let matches = if ty.mutable {
                        global.ty.mutable && global.ty.value == expected
                    } else {
                        !global.ty.mutable && self.type_context().value_subtype(global.ty.value, expected)
                    };
                    if !matches {
                        return link_error("incompatible import type");
                    }
                    globals.push(*address);
                }
                (ImportDescription::Tag(ty), ExternValue::Tag(address)) => {
                    if self.tag(*address).type_identity != type_identities[ty.type_index as usize] {
                        return link_error("incompatible import type");
                    }
                    tags.push(*address);
                }
                _ => return link_error("incompatible import type"),
            }
        }

        // The instance is allocated before its contents, so that its functions can point at its data.
        let instance_address = InstanceAddress(self.instances.len() as u32);
        let mut instance = Box::new(Instance {
            data: Box::new(InstanceData {
                memory0_base: std::ptr::null_mut(),
                memories: std::ptr::null(),
                globals: std::ptr::null(),
                functions: std::ptr::null(),
                tables: std::ptr::null(),
                type_identities: std::ptr::null(),
                data_segments: std::ptr::null(),
                element_segments: std::ptr::null(),
                tags: std::ptr::null(),
                function_count: 0,
                memory_count: 0,
            }),
            module: module.clone(),
            functions: Vec::new(),
            tables: Vec::new(),
            memories: Vec::new(),
            globals: Vec::new(),
            exports: HashMap::new(),
            type_identities,
            element_storage: Vec::new(),
            element_segments: Vec::new(),
            data_segments: module
                .data
                .iter()
                .map(|data| Segment {
                    contents: module.bytes.as_ptr().wrapping_add(data.bytes.start as usize),
                    length: u64::from(data.bytes.end - data.bytes.start),
                })
                .collect(),
            function_headers: Vec::new(),
            global_slots: Vec::new(),
            tags: Vec::new(),
            tag_identities: Vec::new(),
            table_datas: Vec::new(),
            memory_datas: Vec::new(),
        });
        let instance_data: *const InstanceData = &raw const *instance.data;

        for (offset, &type_index) in module.functions.iter().enumerate() {
            let info = &module.function_infos[offset];
            let body = &module.code[offset];
            let ty = module.types[type_index as usize]
                .as_func()
                .expect("validation checks function types")
                .clone();
            let local_count = info.local_count;
            // Each local has an 8-byte slot. A v128 local's slot holds the address of its 16-byte storage, which
            // follows the slots, 16-byte aligned like fp.
            let scalar_size = (local_count * 8).next_multiple_of(16);
            let local_types = ty.params.iter().copied().chain(
                body.locals
                    .iter()
                    .flat_map(|&(count, local)| std::iter::repeat_n(local, count as usize)),
            );
            let mut vector_storage = Vec::new();
            let mut locals_size = scalar_size;
            for local in local_types {
                if local == ValType::V128 {
                    vector_storage.push(locals_size);
                    locals_size += 16;
                } else {
                    vector_storage.push(0);
                }
            }
            let has_v128_locals = locals_size != scalar_size;
            if !has_v128_locals {
                vector_storage.clear();
            }
            let vector_storage = vector_storage.into_boxed_slice();
            let frame_size =
                (FRAME_HEADER_SIZE as u32 + info.max_stack_slots * 8 + SPILL_AREA_SIZE as u32).next_multiple_of(16);
            let address = FunctionAddress(self.functions.len() as u32);
            let code = module.bytes.as_ptr().wrapping_add(body.code.start as usize);
            let code_end = module.bytes.as_ptr().wrapping_add(body.code.end as usize - 1);
            self.functions.push(Box::new(FunctionInstance {
                header: FunctionHeader {
                    code,
                    code_end,
                    side_table: info.side_table.as_ptr().cast(),
                    instance: instance_data,
                    kind: if has_v128_locals {
                        FUNCTION_KIND_WASM_VECTOR
                    } else {
                        FUNCTION_KIND_WASM
                    },
                    param_slots: ty.params.iter().map(|param| param.slot_count()).sum(),
                    result_slots: ty.results.iter().map(|result| result.slot_count()).sum(),
                    local_count,
                    locals_size,
                    reserved: 0,
                    frame_size,
                    type_identity: instance.type_identities[type_index as usize],
                    host: std::ptr::null(),
                    vector_storage: if has_v128_locals {
                        vector_storage.as_ptr()
                    } else {
                        std::ptr::null()
                    },
                },
                address,
                ty,
                vector_storage,
                kind: FunctionKind::Wasm {
                    module: module.clone(),
                    index: module.imported_function_count + offset as u32,
                    instance: instance_address,
                },
            }));
            functions.push(address);
        }
        instance.functions = functions;
        instance.function_headers = instance
            .functions
            .iter()
            .map(|address| &raw const self.function(*address).header)
            .collect();

        for memory in &module.memories {
            let address = self
                .allocate_memory(*memory)
                .ok_or_else(|| InstantiationError::Link("cannot allocate memory".to_string()))?;
            memories.push(address);
        }
        instance.memories = memories;

        // Globals are initialized in order, and an initializer may read the globals before its own.
        for global in &module.globals {
            let value = constant::evaluate(
                self,
                &globals,
                &instance.functions,
                &instance.type_identities,
                &module,
                &global.initializer,
            )
            .map_err(InstantiationError::Trap)?;
            let ty = GlobalType {
                value: canonical(global.ty.value),
                mutable: global.ty.mutable,
            };
            globals.push(self.allocate_global(ty, value));
        }
        instance.globals = globals;

        for table in &module.tables {
            let initial = match &table.initializer {
                Some(initializer) => {
                    match constant::evaluate(
                        self,
                        &instance.globals,
                        &instance.functions,
                        &instance.type_identities,
                        &module,
                        initializer,
                    )
                    .map_err(InstantiationError::Trap)?
                    {
                        Value::Ref(bits) => bits,
                        _ => 0,
                    }
                }
                None => 0,
            };
            let ValType::Ref(element) = canonical(ValType::Ref(table.ty.element)) else {
                unreachable!("table elements are references");
            };
            tables.push(self.allocate_table(TableType { element, ..table.ty }, initial));
        }
        instance.tables = tables;
        for tag in &module.tags {
            let ty = module.types[tag.type_index as usize]
                .as_func()
                .expect("validation checks tag types")
                .clone();
            let type_identity = instance.type_identities[tag.type_index as usize];
            tags.push(self.allocate_tag_with_identity(ty, type_identity));
        }
        instance.tags = tags;
        self.refresh_instance_data(&mut instance);

        for export in &module.exports {
            let value = match export.kind {
                ExternalKind::Function => ExternValue::Function(instance.functions[export.index as usize]),
                ExternalKind::Table => ExternValue::Table(instance.tables[export.index as usize]),
                ExternalKind::Memory => ExternValue::Memory(instance.memories[export.index as usize]),
                ExternalKind::Global => ExternValue::Global(instance.globals[export.index as usize]),
                ExternalKind::Tag => ExternValue::Tag(instance.tags[export.index as usize]),
            };
            instance.exports.insert(export.name.clone(), value);
        }

        // Element segments.
        let mut element_values = Vec::with_capacity(module.elements.len());
        for element in &module.elements {
            let values = match &element.items {
                ElementItems::Functions(indices) => indices
                    .iter()
                    .map(|&index| self.function_reference(instance.functions[index as usize]))
                    .collect::<Vec<_>>(),
                ElementItems::Expressions(expressions) => {
                    let mut values = Vec::with_capacity(expressions.len());
                    for expression in expressions {
                        match constant::evaluate(
                            self,
                            &instance.globals,
                            &instance.functions,
                            &instance.type_identities,
                            &module,
                            expression,
                        )
                        .map_err(InstantiationError::Trap)?
                        {
                            Value::Ref(bits) => values.push(bits),
                            _ => values.push(0),
                        }
                    }
                    values
                }
            };
            element_values.push(values);
        }
        instance.element_storage = element_values;
        instance.element_segments = instance
            .element_storage
            .iter()
            .zip(&module.elements)
            .map(|(values, element)| Segment {
                contents: values.as_ptr().cast(),
                length: if matches!(element.mode, ElementMode::Passive) {
                    values.len() as u64
                } else {
                    0
                },
            })
            .collect();
        instance.data.element_segments = instance.element_segments.as_ptr();
        instance.data.data_segments = instance.data_segments.as_ptr();
        self.instances.push(instance);
        let instance_index = instance_address.0 as usize;

        for (index, element) in module.elements.iter().enumerate() {
            if let ElementMode::Active { table, offset } = &element.mode {
                let offset = self.evaluate_offset(instance_index, &module, offset)?;
                let table_address = self.instances[instance_index].tables[*table as usize];
                let values = std::mem::take(&mut self.instances[instance_index].element_storage[index]);
                let table = self.table_mut(table_address);
                let end = offset.checked_add(values.len() as u64);
                if end.is_none_or(|end| end > table.elements.len() as u64) {
                    return Err(InstantiationError::Trap(Trap::TableOutOfBounds));
                }
                table.elements[offset as usize..offset as usize + values.len()].copy_from_slice(&values);
            }
        }

        for (index, data) in module.data.iter().enumerate() {
            if let DataMode::Active { memory, offset } = &data.mode {
                let offset = self.evaluate_offset(instance_index, &module, offset)?;
                let memory_address = self.instances[instance_index].memories[*memory as usize];
                let bytes = module.bytes_of(&data.bytes);
                let memory = self.memory_mut(memory_address);
                let end = offset.checked_add(bytes.len() as u64);
                if end.is_none_or(|end| end > memory.size() as u64) {
                    return Err(InstantiationError::Trap(Trap::MemoryOutOfBounds));
                }
                memory.bytes_mut()[offset as usize..offset as usize + bytes.len()].copy_from_slice(bytes);
                self.instances[instance_index].data_segments[index].length = 0;
            }
        }

        if let Some(start) = module.start {
            let function = self.instances[instance_index].functions[start as usize];
            self.invoke(function, &[]).map_err(InstantiationError::Trap)?;
        }
        Ok(instance_address)
    }

    fn evaluate_offset(
        &mut self,
        instance_index: usize,
        module: &Module,
        expression: &crate::module::ConstantExpression,
    ) -> Result<u64, InstantiationError> {
        let instance = &self.instances[instance_index];
        let (globals, functions, type_identities) = (
            instance.globals.clone(),
            instance.functions.clone(),
            instance.type_identities.clone(),
        );
        match constant::evaluate(self, &globals, &functions, &type_identities, module, expression)
            .map_err(InstantiationError::Trap)?
        {
            Value::I32(value) => Ok(u64::from(value as u32)),
            Value::I64(value) => Ok(value as u64),
            _ => Ok(0),
        }
    }

    /// Point the instance's interpreter-visible data at its current contents.
    fn refresh_instance_data(&self, instance: &mut Instance) {
        instance.function_headers = instance
            .functions
            .iter()
            .map(|address| &raw const self.function(*address).header)
            .collect();
        instance.global_slots = instance
            .globals
            .iter()
            .map(|address| self.globals[address.0 as usize].value.as_ptr().cast_mut())
            .collect();
        instance.table_datas = instance
            .tables
            .iter()
            .map(|address| (&raw const *self.tables[address.0 as usize].data).cast_mut())
            .collect();
        instance.memory_datas = instance
            .memories
            .iter()
            .map(|address| (&raw const *self.memories[address.0 as usize].data).cast_mut())
            .collect();
        let data = &mut instance.data;
        data.functions = instance.function_headers.as_ptr();
        data.function_count = instance.function_headers.len() as u32;
        data.globals = instance.global_slots.as_ptr();
        data.tables = instance.table_datas.as_ptr();
        data.memories = instance.memory_datas.as_ptr();
        data.memory_count = instance.memory_datas.len() as u32;
        data.memory0_base = instance
            .memories
            .first()
            .map_or(std::ptr::null_mut(), |address| self.memory(*address).data.base);
        data.type_identities = instance.type_identities.as_ptr();
        instance.tag_identities = instance
            .tags
            .iter()
            .map(|address| (&raw const *self.tags[address.0 as usize]) as u64)
            .collect();
        instance.data.tags = instance.tag_identities.as_ptr();
    }

    pub fn tag(&self, address: TagAddress) -> &TagInstance {
        &self.tags[address.0 as usize]
    }

    /// Add a tag for exceptions that carry values of the parameter types of `ty`.
    pub fn allocate_tag(&mut self, ty: FuncType) -> TagAddress {
        let type_identity = self.identity_of_function_type(&ty);
        self.allocate_tag_with_identity(ty, type_identity)
    }

    fn allocate_tag_with_identity(&mut self, ty: FuncType, type_identity: u32) -> TagAddress {
        let address = TagAddress(self.tags.len() as u32);
        self.tags.push(Box::new(TagInstance {
            address,
            ty,
            type_identity,
        }));
        address
    }

    /// The tag of an exception reference's identity.
    pub fn tag_from_identity(&self, identity: u64) -> TagAddress {
        unsafe { (*(identity as *const TagInstance)).address }
    }

    /// Make an exception with `tag` and the values of `payload`, and return its reference.
    pub fn allocate_exception(&mut self, tag: TagAddress, payload: Box<[u64]>) -> u64 {
        let exception = Box::new(ExceptionInstance { tag, payload });
        let reference = (&raw const *exception) as u64;
        self.exceptions.push(exception);
        reference
    }

    /// The exception an exception reference refers to.
    pub fn exception(&self, reference: u64) -> &ExceptionInstance {
        unsafe { &*(reference as *const ExceptionInstance) }
    }

    pub fn function_reference(&self, address: FunctionAddress) -> u64 {
        (&raw const self.function(address).header) as u64
    }

    /// The function a non-null function reference refers to.
    pub fn function_from_reference(&self, reference: u64) -> Option<FunctionAddress> {
        if reference == 0 {
            return None;
        }
        let function = unsafe { &*(reference as *const FunctionInstance) };
        Some(function.address)
    }

    pub fn invoke(&mut self, address: FunctionAddress, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let function = self.function(address);
        if let FunctionKind::Host(_) = function.kind {
            return self.call_host(address, arguments);
        }
        let ty = function.ty.clone();
        let header: *const FunctionHeader = &raw const function.header;
        // The arguments are laid out as they will be on the operand stack, which grows down: the last one first.
        let mut argument_slots = Vec::new();
        for argument in arguments.iter().rev() {
            argument.push_slots(&mut argument_slots);
        }
        let mut result_slots = vec![0u64; function.header.result_slots as usize];
        let is_outermost = self.context.entry_record.is_null();
        if is_outermost {
            self.context.stack_limit = interpreter::stack_limit(STACK_RESERVE);
        }
        self.context.store = (self as *mut Store).cast();
        let mut record = EntryRecord {
            function: header,
            arguments: argument_slots.as_ptr(),
            results: result_slots.as_mut_ptr(),
            context: &raw mut *self.context,
            trap: 0,
            padding: 0,
            saved_entry_fp: 0,
            saved_entry_record: std::ptr::null_mut(),
        };
        unsafe { interpreter::enter(&mut record) };
        if record.trap != 0 {
            return Err(Trap::from_code(record.trap, self.pending_trap.take()));
        }
        // The results are laid out as the operand stack holds them, the first one in the highest slots.
        let mut results = Vec::with_capacity(ty.results.len());
        let mut slot = result_slots.len();
        for &result in &ty.results {
            slot -= result.slot_count() as usize;
            results.push(Value::from_slots(result, &result_slots[slot..]));
        }
        Ok(results)
    }

    fn call_host(&mut self, address: FunctionAddress, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let FunctionKind::Host(callback) = &self.functions[address.0 as usize].kind else {
            unreachable!("only host functions are called through call_host")
        };
        let callback = callback.clone();
        callback(self, arguments)
    }
}

fn limits_match(current: u64, current_max: Option<u64>, expected: &crate::types::Limits) -> bool {
    if current < expected.min {
        return false;
    }
    match expected.max {
        None => true,
        Some(max) => current_max.is_some_and(|current_max| current_max <= max),
    }
}
