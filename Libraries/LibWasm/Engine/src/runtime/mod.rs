/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime: the store, which holds what the entities of a thread share (types, runtime types, the interpreter's
//! context), the entities themselves (see entity.rs), instantiation, and calling into the interpreter.

/// Export the runtime functions that the interpreter and compiled code call, under the names they call them by, with
/// the System V calling convention on every x86-64 target, Windows included.
macro_rules! export_runtime_functions {
    ($($symbol:ident => $function:ident),* $(,)?) => {
        $(
            /// # Safety
            ///
            /// Only the interpreter and compiled code call this, with the context of the store they run in.
            #[cfg(all(target_arch = "x86_64", windows))]
            #[unsafe(no_mangle)]
            pub unsafe extern "sysv64" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }

            /// # Safety
            ///
            /// Only the interpreter and compiled code call this, with the context of the store they run in.
            #[cfg(not(all(target_arch = "x86_64", windows)))]
            #[unsafe(no_mangle)]
            pub unsafe extern "C" fn $symbol(context: *mut ExecutionContext) -> u64 {
                unsafe { $function(context) }
            }
        )*
    };
}

pub mod constant;
pub mod entity;
pub mod gc;
pub mod heap;
pub mod helpers;
pub mod interpreter;
pub mod jit;
pub mod memory;
pub mod ranges;

use crate::abi::{
    EntryRecord, ExecutionContext, FRAME_HEADER_SIZE, FUNCTION_KIND_HOST, FUNCTION_KIND_WASM,
    FUNCTION_KIND_WASM_VECTOR, FunctionHeader, InstanceData, SPILL_AREA_SIZE, Segment, TableData,
};
use crate::module::{DataMode, ElementItems, ElementMode, ExternalKind, ImportDescription, Module};
use crate::types::{FuncType, GlobalType, HeapType, RefType, TableType, TypeCanonicalizer, ValType};
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

    /// Whether the value can be one of type `ty`: one of its kind, and not null where `ty` is not nullable.
    ///
    /// NB: The heap type of a reference is the embedder's to check, which knows what the reference is. Its bits do
    ///     not say which hierarchy it is of: an externref keeps the bits of the anyref it externalizes.
    pub fn matches(self, ty: ValType) -> bool {
        match (self, ty) {
            (Self::I32(_), ValType::I32)
            | (Self::I64(_), ValType::I64)
            | (Self::F32(_), ValType::F32)
            | (Self::F64(_), ValType::F64)
            | (Self::V128(_), ValType::V128) => true,
            (Self::Ref(bits), ValType::Ref(ty)) => bits != 0 || ty.nullable,
            _ => false,
        }
    }

    /// The value's slots in memory order (a v128's low half first), and how many of the two it takes.
    pub fn to_slots(self) -> ([u64; 2], usize) {
        match self {
            Self::I32(value) => ([u64::from(value as u32), 0], 1),
            Self::I64(value) => ([value as u64, 0], 1),
            Self::F32(bits) => ([u64::from(bits), 0], 1),
            Self::F64(bits) | Self::Ref(bits) => ([bits, 0], 1),
            Self::V128(bits) => ([bits as u64, (bits >> 64) as u64], 2),
        }
    }

    /// Append the slots of this value in memory order: a v128's low half first.
    pub fn push_slots(self, slots: &mut Vec<u64>) {
        let (value, count) = self.to_slots();
        slots.extend_from_slice(&value[..count]);
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

/// Whether `values` can be values of `types`, one of each.
fn values_match(values: &[Value], types: &[ValType]) -> bool {
    values.len() == types.len() && values.iter().zip(types).all(|(value, &ty)| value.matches(ty))
}

/// Declares `Trap`, with a variant for each trap that has a `TRAP_*` code and a message of its own, besides traps of
/// the host and uncaught exceptions.
macro_rules! traps {
    ($($variant:ident = $code:ident, $message:literal;)*) => {
        #[derive(Debug, Clone, PartialEq, Eq)]
        pub enum Trap {
            $($variant,)*
            Host(String),
            UncaughtException(u64),
        }

        impl Trap {
            fn from_code(code: u32, pending: Option<Trap>) -> Self {
                match code {
                    $(crate::abi::$code => Self::$variant,)*
                    _ => pending
                        .unwrap_or_else(|| Self::Host("the runtime reported a trap without details".to_string())),
                }
            }

            /// The `TRAP_*` code the interpreter and compiled code know the trap by.
            pub fn code(&self) -> u32 {
                match self {
                    $(Self::$variant => crate::abi::$code,)*
                    Self::Host(_) => crate::abi::TRAP_RUNTIME,
                    Self::UncaughtException(_) => crate::abi::TRAP_UNCAUGHT_EXCEPTION,
                }
            }

            pub fn message(&self) -> String {
                match self {
                    $(Self::$variant => $message.to_string(),)*
                    Self::Host(message) => message.clone(),
                    Self::UncaughtException(_) => "uncaught exception".to_string(),
                }
            }
        }
    };
}

traps! {
    Unreachable = TRAP_UNREACHABLE, "unreachable";
    StackExhausted = TRAP_STACK_EXHAUSTED, "call stack exhausted";
    MemoryOutOfBounds = TRAP_MEMORY_OUT_OF_BOUNDS, "Memory access out of bounds";
    IntegerDivideByZero = TRAP_INTEGER_DIVIDE_BY_ZERO, "integer divide by zero";
    IntegerOverflow = TRAP_INTEGER_OVERFLOW, "integer overflow";
    InvalidConversion = TRAP_INVALID_CONVERSION, "invalid conversion to integer";
    UndefinedElement = TRAP_UNDEFINED_ELEMENT, "undefined element";
    UninitializedElement = TRAP_UNINITIALIZED_ELEMENT, "uninitialized element";
    IndirectCallTypeMismatch = TRAP_INDIRECT_CALL_TYPE_MISMATCH, "indirect call type mismatch";
    TableOutOfBounds = TRAP_TABLE_OUT_OF_BOUNDS, "out of bounds table access";
    NullReference = TRAP_NULL_REFERENCE, "null reference";
    CastFailure = TRAP_CAST_FAILURE, "cast failure";
    ArrayOutOfBounds = TRAP_ARRAY_OUT_OF_BOUNDS, "out of bounds array access";
    UnalignedAtomic = TRAP_UNALIGNED_ATOMIC, "Unaligned atomic memory access";
    ExpectedSharedMemory = TRAP_EXPECTED_SHARED_MEMORY, "Expected shared memory";
}

pub type FunctionAddress = entity::Handle<FunctionInstance>;
pub type TableAddress = entity::Handle<TableInstance>;
pub type MemoryAddress = entity::Handle<MemoryInstance>;
pub type GlobalAddress = entity::Handle<GlobalInstance>;
pub type InstanceAddress = entity::Handle<Instance>;
pub type TagAddress = entity::Handle<TagInstance>;

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
    /// The types of the values an exception with this tag carries.
    pub ty: FuncType,
    pub type_identity: u32,
}

/// A function the embedder implements.
pub trait HostFunction {
    /// Get the arguments and return the results, in the function type's order.
    fn call(&self, store: &mut Store, arguments: &[Value]) -> Result<Vec<Value>, Trap>;

    /// Visit the cells the function refers to, which its function keeps alive.
    fn visit_edges(&self, _visitor: &heap::Visitor) {}

    /// The embedder's data of a function made through the C interface.
    fn user_data(&self) -> *mut core::ffi::c_void {
        core::ptr::null_mut()
    }
}

impl<F: Fn(&mut Store, &[Value]) -> Result<Vec<Value>, Trap>> HostFunction for F {
    fn call(&self, store: &mut Store, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        self(store, arguments)
    }
}

pub type HostCallback = Rc<dyn HostFunction>;

pub enum FunctionKind {
    Wasm {
        module: Arc<Module>,
        index: u32,
        instance: InstanceAddress,
    },
    Host(HostCallback),
}

/// A function. The interpreter sees its header, which is its first field, so that a function reference, which points
/// at the header, is the function's handle.
#[repr(C)]
pub struct FunctionInstance {
    pub header: FunctionHeader,
    pub ty: FuncType,
    /// What `header.vector_storage` points at.
    pub vector_storage: Box<[u32]>,
    pub kind: FunctionKind,
    /// Where compiled code passes its values, once a call between the tiers needed it.
    pub native_shapes: std::cell::OnceCell<Box<jit::NativeShapes>>,
}

/// A table. The interpreter sees its data, which is its first field and so at its handle.
#[repr(C)]
pub struct TableInstance {
    pub data: TableData,
    pub ty: TableType,
    pub elements: Vec<u64>,
    /// How the collector follows the elements (TRACE_*).
    pub trace: u8,
}

impl TableInstance {
    fn new(ty: TableType, initial: u64, trace: u8) -> Self {
        let mut elements = vec![initial; ty.limits.min as usize];
        let data = TableData {
            elements: elements.as_mut_ptr(),
            size: elements.len() as u64,
            address64: u64::from(ty.limits.address_type == crate::types::AddressType::I64),
        };
        Self {
            data,
            ty,
            elements,
            trace,
        }
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

/// A global. The interpreter sees its value's slots, which are its first field and so at its handle.
#[repr(C, align(16))]
pub struct GlobalInstance {
    /// The value's slots: one, or two for v128.
    pub value: [u64; 2],
    pub ty: GlobalType,
    /// How the collector follows the value (TRACE_*).
    pub trace: u8,
}

impl GlobalInstance {
    pub fn get(&self) -> Value {
        Value::from_slots(self.ty.value, &self.value)
    }

    pub fn set(&mut self, value: Value) {
        let (slots, count) = value.to_slots();
        self.value[..count].copy_from_slice(&slots[..count]);
    }
}

/// An instance. The interpreter sees its data, which is its first field and so at its handle.
#[repr(C)]
pub struct Instance {
    pub data: InstanceData,
    pub module: Arc<Module>,
    pub functions: Vec<FunctionAddress>,
    pub tables: Vec<TableAddress>,
    pub memories: Vec<MemoryAddress>,
    pub globals: Vec<GlobalAddress>,
    pub tags: Vec<TagAddress>,
    pub exports: HashMap<String, ExternValue>,
    /// The identity of each of the module's types, which the instances of a module share.
    pub type_identities: Rc<[u32]>,
    /// The references of each passive element segment. The interpreter sees them through `element_segments`.
    pub element_storage: Vec<Vec<u64>>,
    /// How the collector follows the references of each element segment (TRACE_*).
    pub element_traces: Vec<u8>,
    /// Each element segment, empty once dropped or used by instantiation.
    pub element_segments: Vec<Segment>,
    /// Each data segment, empty once dropped or used by instantiation.
    pub data_segments: Vec<Segment>,
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

/// What the entities of a thread share. Its heap holds the entities, which the store does not refer to: they live
/// while their embedder, other entities, or running code refer to them, and belong to the store that made them.
pub struct Store {
    context: Box<ExecutionContext>,
    /// Where entities, GC objects, exceptions, runtime types, and memories live.
    pub heap: heap::Heap,
    /// The cell through which the heap traces the runtime types, which the store keeps for as long as it lives.
    pub(crate) root_cell: *mut u8,
    /// The root that keeps `root_cell` alive.
    root: Option<*mut core::ffi::c_void>,
    /// The runtime type of each type identity, in the cells the heap keeps them in.
    rtt_pointers: Vec<*const crate::abi::Rtt>,
    canonicalizer: TypeCanonicalizer,
    /// The type of each store-wide type identity, with its type indices replaced by identities, so that types of
    /// different modules can be compared.
    types: Vec<crate::types::SubType>,
    /// A trap a runtime function reported, which the interpreter only knows as TRAP_RUNTIME.
    pending_trap: Option<Trap>,
    /// The module `module_type_identities_of()` was last asked about, and its identities. An embedder asks about
    /// one module many times in a row while it links its imports.
    last_module_identities: Option<(std::sync::Weak<Module>, Rc<[u32]>)>,
    /// What runs after `memory.grow` grows a memory.
    pub memory_grow_callback: Option<Rc<dyn Fn(MemoryAddress)>>,
    /// Whether the agent the store runs code for may block, its [[CanBlock]]: `memory.atomic.wait` traps where it
    /// may not. The embedder decides (a browser's window agents may not block).
    pub can_block: bool,
    /// The store's compiled code, once it has some.
    jit: Option<jit::StoreJit>,
}

impl Default for Store {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for Store {
    fn drop(&mut self) {
        if !self.root_cell.is_null() {
            gc::detach_store_root(self.root_cell);
        }
        if let (Some(root), Some(heap)) = (self.root.take(), self.heap.gc()) {
            heap.unroot(root);
        }
        self.heap.drop_standalone_entities();
    }
}

impl Store {
    /// A store whose GC objects and memories are plain allocations that are never freed.
    pub fn new() -> Self {
        Self::with_heap(None)
    }

    /// A store whose entities and GC objects live in the heap of `interface`, and whose memories live in its
    /// primitive storage. The store must stay where it is once `attach_to_heap()` gives it a root cell.
    pub fn with_heap(interface: Option<heap::GcInterface>) -> Self {
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
                exception: 0,
                resume: [0; 3],
            }),
            canonicalizer: TypeCanonicalizer::new(),
            types: Vec::new(),
            pending_trap: None,
            last_module_identities: None,
            heap: heap::Heap::new(interface),
            root_cell: std::ptr::null_mut(),
            root: None,
            rtt_pointers: Vec::new(),
            memory_grow_callback: None,
            can_block: true,
            jit: None,
        }
    }

    /// Have the heap keep the runtime types for as long as the store lives. The store must not move after this.
    pub fn attach_to_heap(&mut self) {
        let Some(heap) = self.heap.gc() else {
            return;
        };
        self.root_cell = gc::allocate_store_root(&self.heap, self);
        self.root = Some(heap.root(self.root_cell));
    }

    // NB: The accessors below trust that the entity lives, which its embedder, or the instance it belongs to,
    //     guarantees while it uses it.

    pub fn function(&self, address: FunctionAddress) -> &FunctionInstance {
        unsafe { address.get() }
    }

    pub fn memory(&self, address: MemoryAddress) -> &MemoryInstance {
        unsafe { address.get() }
    }

    pub fn memory_mut(&mut self, address: MemoryAddress) -> &mut MemoryInstance {
        unsafe { address.get_mut() }
    }

    pub fn table(&self, address: TableAddress) -> &TableInstance {
        unsafe { address.get() }
    }

    pub fn table_mut(&mut self, address: TableAddress) -> &mut TableInstance {
        unsafe { address.get_mut() }
    }

    pub fn global(&self, address: GlobalAddress) -> &GlobalInstance {
        unsafe { address.get() }
    }

    pub fn global_mut(&mut self, address: GlobalAddress) -> &mut GlobalInstance {
        unsafe { address.get_mut() }
    }

    pub fn instance(&self, address: InstanceAddress) -> &Instance {
        unsafe { address.get() }
    }

    /// The store-wide identity of a function type that is not part of a recursion group.
    fn identity_of_function_type(&mut self, ty: &FuncType) -> u32 {
        let sub_type = crate::types::SubType {
            is_final: true,
            supertypes: Vec::new(),
            composite: crate::types::CompositeType::Func(ty.clone()),
        };
        // The type refers to other types by their identities, and to none in its group.
        let identities = self
            .canonicalizer
            .canonicalize_group(std::slice::from_ref(&sub_type), u32::MAX, |identity| identity);
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
        let first = self.types.len() as u32;
        for sub_type in group {
            self.types.push(sub_type.clone());
        }
        // Types of a group refer to each other, so they all have to be there before their runtime types are made.
        for identity in first..self.types.len() as u32 {
            let rtt = gc::allocate_rtt(self, identity);
            self.rtt_pointers.push(rtt);
        }
        self.context.rtts = self.rtt_pointers.as_ptr();
    }

    /// Give the types of `module` store-wide identities, and return the identity of each.
    pub fn module_type_identities(&mut self, module: &Module) -> Vec<u32> {
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
        type_identities
    }

    /// `module_type_identities()`, remembered for the last module asked about.
    pub fn module_type_identities_of(&mut self, module: &Arc<Module>) -> Rc<[u32]> {
        if let Some((last, identities)) = &self.last_module_identities
            && last.upgrade().is_some_and(|last| Arc::ptr_eq(&last, module))
        {
            return identities.clone();
        }
        let identities: Rc<[u32]> = self.module_type_identities(module).into();
        self.last_module_identities = Some((Arc::downgrade(module), identities.clone()));
        identities
    }

    /// Whether a non-null reference matches `heap`, whose concrete types are identities. References of the any
    /// hierarchy must be in their internal form: i31 references, GC objects, or host references.
    pub fn reference_matches(&self, reference: u64, heap: HeapType) -> bool {
        let tag = reference & 3;
        let object_rtt = || unsafe { &**(reference as *const *const crate::abi::Rtt) };
        let is_subtype = |actual: &crate::abi::Rtt, identity: u32| {
            let target = self.rtt(identity);
            actual.depth >= target.depth && unsafe { *actual.display.add(target.depth as usize) } == identity
        };
        match heap {
            HeapType::Any | HeapType::Extern | HeapType::Func | HeapType::Exn => true,
            HeapType::Eq => reference & 1 != 0 || tag == 0,
            HeapType::I31 => reference & 1 != 0,
            HeapType::Struct => tag == 0 && object_rtt().kind == crate::abi::RTT_KIND_STRUCT,
            HeapType::Array => tag == 0 && object_rtt().kind == crate::abi::RTT_KIND_ARRAY,
            HeapType::Concrete(identity) => {
                if self.rtt(identity).kind == crate::abi::RTT_KIND_FUNCTION {
                    let function = unsafe { &*(reference as *const FunctionHeader) };
                    is_subtype(self.rtt(function.type_identity), identity)
                } else {
                    tag == 0 && is_subtype(object_rtt(), identity)
                }
            }
            HeapType::None | HeapType::NoFunc | HeapType::NoExtern | HeapType::NoExn | HeapType::Bottom => false,
        }
    }

    /// Whether the type `identity` is a function type, as opposed to a struct or array type.
    pub fn is_function_type(&self, identity: u32) -> bool {
        self.rtt(identity).kind == crate::abi::RTT_KIND_FUNCTION
    }

    /// The types of the store, where concrete heap types are identities.
    fn type_context(&self) -> crate::validator::TypeContext<'_> {
        crate::validator::TypeContext {
            types: &self.types,
            identities: None,
        }
    }

    pub fn allocate_host_function(&mut self, ty: FuncType, callback: HostCallback) -> FunctionAddress {
        let type_identity = self.identity_of_function_type(&ty);
        let function = entity::allocate(
            &self.heap,
            FunctionInstance {
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
                    tier_up_budget: jit::initial_tier_up_budget(),
                    frame_size: 0,
                    type_identity,
                    host: std::ptr::null(),
                    vector_storage: std::ptr::null(),
                    native_entry: jit::initial_native_entry(),
                },
                ty,
                vector_storage: Box::new([]),
                kind: FunctionKind::Host(callback),
                native_shapes: std::cell::OnceCell::new(),
            },
        );
        unsafe { function.get_mut() }.header.host = function.as_ptr().cast_const().cast();
        function
    }

    pub fn allocate_memory(&mut self, ty: crate::types::MemoryType) -> Option<MemoryAddress> {
        let memory = MemoryInstance::new(ty, &self.heap)?;
        Some(entity::allocate(&self.heap, memory))
    }

    /// Add a table of `ty.limits.min` elements set to `initial`. Returns None if it would be too large.
    pub fn allocate_table(&mut self, ty: TableType, initial: u64) -> Option<TableAddress> {
        if ty.limits.min > MAX_TABLE_SIZE {
            return None;
        }
        let trace = gc::trace_of(ValType::Ref(ty.element), &self.types);
        Some(entity::allocate(&self.heap, TableInstance::new(ty, initial, trace)))
    }

    pub fn allocate_global(&mut self, ty: GlobalType, value: Value) -> GlobalAddress {
        let mut global = GlobalInstance {
            value: [0; 2],
            ty,
            trace: gc::trace_of(ty.value, &self.types),
        };
        global.set(value);
        entity::allocate(&self.heap, global)
    }

    pub fn instantiate(
        &mut self,
        module: Arc<Module>,
        imports: &[ExternValue],
    ) -> Result<InstanceAddress, InstantiationError> {
        // Instantiation keeps references in places the collector does not look, like the values of constant
        // expressions on their way into globals and segments. The start function runs once they are in place.
        self.heap.defer_gc();
        let instance = self.instantiate_without_starting(module, imports);
        self.heap.undefer_gc();
        let (instance, start) = instance?;
        jit::instantiated(self, instance);
        if let Some(start) = start {
            self.invoke(start, &[]).map_err(InstantiationError::Trap)?;
        }
        Ok(instance)
    }

    fn instantiate_without_starting(
        &mut self,
        module: Arc<Module>,
        imports: &[ExternValue],
    ) -> Result<(InstanceAddress, Option<FunctionAddress>), InstantiationError> {
        let link_error = |message: &str| Err(InstantiationError::Link(message.to_string()));
        if imports.len() != module.imports.len() {
            return link_error("wrong number of imports");
        }

        // NB: The embedder usually asked about the module's types while it linked its imports.
        let type_identities = self.module_type_identities_of(&module);

        // Types of the store have identities where the module's types have indices.
        let canonical = |value: ValType| value.map_type_indices(&|index| type_identities[index as usize]);
        let canonical_function_type = |ty: &FuncType| FuncType {
            params: ty.params.iter().map(|&param| canonical(param)).collect(),
            results: ty.results.iter().map(|&result| canonical(result)).collect(),
        };
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
        let instance_address = entity::allocate(
            &self.heap,
            Instance {
                data: InstanceData {
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
                    context: &raw mut *self.context,
                },
                module: module.clone(),
                functions: Vec::new(),
                tables: Vec::new(),
                memories: Vec::new(),
                globals: Vec::new(),
                exports: HashMap::new(),
                type_identities: type_identities.clone(),
                element_storage: Vec::new(),
                element_traces: Vec::new(),
                element_segments: Vec::new(),
                data_segments: module
                    .data
                    .iter()
                    .map(|data| Segment {
                        contents: module.bytes.as_ptr().wrapping_add(data.bytes.start as usize),
                        length: u64::from(data.bytes.end - data.bytes.start),
                    })
                    .collect(),
                tags: Vec::new(),
            },
        );
        let instance = unsafe { instance_address.get_mut() };
        let instance_data: *const InstanceData = instance_address.as_ptr().cast_const().cast();

        for (offset, &type_index) in module.functions.iter().enumerate() {
            let info = &module.function_infos[offset];
            let body = &module.code[offset];
            let ty = canonical_function_type(
                module.types[type_index as usize]
                    .as_func()
                    .expect("validation checks function types"),
            );
            let local_count = info.local_count;
            // Each local has an 8-byte slot. A v128 local's slot holds the address of its 16-byte storage, which
            // follows the slots, 16-byte aligned like fp.
            let scalar_size = (local_count * 8).next_multiple_of(16);
            // The locals of legacy exception handlers hold exception references.
            let local_types = ty
                .params
                .iter()
                .copied()
                .chain(
                    body.locals
                        .iter()
                        .flat_map(|&(count, local)| std::iter::repeat_n(local, count as usize)),
                )
                .chain(std::iter::repeat_n(
                    ValType::Ref(RefType::EXNREF),
                    info.exception_local_count as usize,
                ));
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
            let code = module.bytes.as_ptr().wrapping_add(body.code.start as usize);
            let code_end = module.bytes.as_ptr().wrapping_add(body.code.end as usize - 1);
            let address = entity::allocate(
                &self.heap,
                FunctionInstance {
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
                        tier_up_budget: jit::initial_tier_up_budget(),
                        frame_size,
                        type_identity: instance.type_identities[type_index as usize],
                        host: std::ptr::null(),
                        vector_storage: if has_v128_locals {
                            vector_storage.as_ptr()
                        } else {
                            std::ptr::null()
                        },
                        native_entry: jit::initial_native_entry(),
                    },
                    ty,
                    vector_storage,
                    kind: FunctionKind::Wasm {
                        module: module.clone(),
                        index: module.imported_function_count + offset as u32,
                        instance: instance_address,
                    },
                    native_shapes: std::cell::OnceCell::new(),
                },
            );
            functions.push(address);
        }
        instance.functions = functions;

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
            let address = self
                .allocate_table(TableType { element, ..table.ty }, initial)
                .ok_or_else(|| InstantiationError::Link("cannot allocate table".to_string()))?;
            tables.push(address);
        }
        instance.tables = tables;
        for tag in &module.tags {
            let ty = canonical_function_type(
                module.types[tag.type_index as usize]
                    .as_func()
                    .expect("validation checks tag types"),
            );
            let type_identity = instance.type_identities[tag.type_index as usize];
            tags.push(self.allocate_tag_with_identity(ty, type_identity));
        }
        instance.tags = tags;
        refresh_instance_data(instance);

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
        instance.element_traces = module
            .elements
            .iter()
            .map(|element| {
                let ty = ValType::Ref(element.ty).map_type_indices(&|index| instance.type_identities[index as usize]);
                gc::trace_of(ty, &self.types)
            })
            .collect();
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

        for (index, element) in module.elements.iter().enumerate() {
            if let ElementMode::Active { table, offset } = &element.mode {
                let offset = self.evaluate_offset(instance_address, &module, offset)?;
                let instance = unsafe { instance_address.get_mut() };
                let table_address = instance.tables[*table as usize];
                let values = std::mem::take(&mut instance.element_storage[index]);
                // NB: The segment no longer owns the values, so it does not point at them either.
                instance.element_segments[index].contents = std::ptr::null();
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
                let offset = self.evaluate_offset(instance_address, &module, offset)?;
                let memory_address = self.instance(instance_address).memories[*memory as usize];
                let bytes = module.bytes_of(&data.bytes);
                let memory = self.memory_mut(memory_address);
                let end = offset.checked_add(bytes.len() as u64);
                if end.is_none_or(|end| end > memory.size() as u64) {
                    return Err(InstantiationError::Trap(Trap::MemoryOutOfBounds));
                }
                memory.bytes_mut()[offset as usize..offset as usize + bytes.len()].copy_from_slice(bytes);
                unsafe { instance_address.get_mut() }.data_segments[index].length = 0;
            }
        }

        let start = module
            .start
            .map(|start| self.instance(instance_address).functions[start as usize]);
        Ok((instance_address, start))
    }

    fn evaluate_offset(
        &mut self,
        instance: InstanceAddress,
        module: &Module,
        expression: &crate::module::ConstantExpression,
    ) -> Result<u64, InstantiationError> {
        // NB: Evaluating may allocate in the store, but leaves the instance's vectors as they are.
        let instance = unsafe { instance.get() };
        match constant::evaluate(
            self,
            &instance.globals,
            &instance.functions,
            &instance.type_identities,
            module,
            expression,
        )
        .map_err(InstantiationError::Trap)?
        {
            Value::I32(value) => Ok(u64::from(value as u32)),
            Value::I64(value) => Ok(value as u64),
            _ => Ok(0),
        }
    }

    pub fn tag(&self, address: TagAddress) -> &TagInstance {
        unsafe { address.get() }
    }

    /// Add a tag for exceptions that carry values of the parameter types of `ty`.
    pub fn allocate_tag(&mut self, ty: FuncType) -> TagAddress {
        let type_identity = self.identity_of_function_type(&ty);
        self.allocate_tag_with_identity(ty, type_identity)
    }

    fn allocate_tag_with_identity(&mut self, ty: FuncType, type_identity: u32) -> TagAddress {
        entity::allocate(&self.heap, TagInstance { ty, type_identity })
    }

    /// Allocate an exception with the tag `tag` that carries `values`, one for each parameter of the tag's type.
    pub fn allocate_exception_with_values(&mut self, tag: TagAddress, values: &[Value]) -> u64 {
        assert!(
            values_match(values, &self.tag(tag).ty.params),
            "an exception carries values of its tag's parameter types"
        );
        // An exception's slots are in operand stack order, the last value first.
        let mut payload = Vec::new();
        for value in values.iter().rev() {
            value.push_slots(&mut payload);
        }
        self.allocate_exception(tag, &payload)
    }

    /// The values an exception carries, one for each parameter of its tag's type.
    pub fn exception_values(&self, reference: u64) -> Vec<Value> {
        let exception = self.exception(reference);
        let params = &self.tag(exception.tag).ty.params;
        let mut values = Vec::with_capacity(params.len());
        let mut slot = exception.payload.len();
        for &param in params {
            slot -= param.slot_count() as usize;
            values.push(Value::from_slots(param, &exception.payload[slot..]));
        }
        values
    }

    /// The tag of a tag identity, which is the tag's handle.
    pub fn tag_from_identity(&self, identity: u64) -> TagAddress {
        unsafe { TagAddress::from_ptr(identity as *const TagInstance) }
    }

    pub fn function_reference(&self, address: FunctionAddress) -> u64 {
        address.as_ptr() as u64
    }

    /// The function a function reference refers to, which is the function's handle.
    pub fn function_from_reference(&self, reference: u64) -> Option<FunctionAddress> {
        if reference == 0 {
            return None;
        }
        Some(unsafe { FunctionAddress::from_ptr(reference as *const FunctionInstance) })
    }

    pub fn invoke(&mut self, address: FunctionAddress, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let function = self.function(address);
        // NB: Code reads the arguments as the function's type says they are, and host functions index them by it.
        if !values_match(arguments, &function.ty.params) {
            return Err(Trap::Host("the arguments do not match the function's type".to_string()));
        }
        if let FunctionKind::Host(_) = function.kind {
            return self.call_host(address, arguments);
        }
        // The arguments are laid out as they will be on the operand stack, which grows down: the last one first.
        let mut argument_slots = Vec::new();
        for argument in arguments.iter().rev() {
            argument.push_slots(&mut argument_slots);
        }
        let mut result_slots = vec![0u64; function.header.result_slots as usize];
        self.invoke_slots(address, &argument_slots, &mut result_slots)?;
        // The results are laid out as the operand stack holds them, the first one in the highest slots.
        let function = self.function(address);
        let mut results = Vec::with_capacity(function.ty.results.len());
        let mut slot = result_slots.len();
        for &result in &function.ty.results {
            slot -= result.slot_count() as usize;
            results.push(Value::from_slots(result, &result_slots[slot..]));
        }
        Ok(results)
    }

    /// Calls the WebAssembly function `address` with the slots of its arguments as the operand stack holds them (the
    /// last argument in the lowest slots), and stores its results' slots the same way in `result_slots`.
    pub fn invoke_slots(
        &mut self,
        address: FunctionAddress,
        argument_slots: &[u64],
        result_slots: &mut [u64],
    ) -> Result<(), Trap> {
        let function = self.function(address);
        debug_assert!(!matches!(function.kind, FunctionKind::Host(_)));
        debug_assert_eq!(argument_slots.len(), function.header.param_slots as usize);
        debug_assert_eq!(result_slots.len(), function.header.result_slots as usize);
        let header: *const FunctionHeader = &raw const function.header;
        let is_outermost = self.context.entry_record.is_null();
        if is_outermost {
            self.context.stack_limit = interpreter::stack_limit(STACK_RESERVE);
        }
        let large_images = jit::large_images_in_use();
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
        // NB: A trap may have skipped calls into compiled code that held large images.
        jit::release_large_images(large_images);
        if record.trap != crate::abi::TRAP_NONE {
            return Err(Trap::from_code(record.trap, self.pending_trap.take()));
        }
        Ok(())
    }

    fn call_host(&mut self, address: FunctionAddress, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let FunctionKind::Host(callback) = &self.function(address).kind else {
            unreachable!("only host functions are called through call_host")
        };
        let callback = callback.clone();
        let values = callback.call(self, arguments)?;
        // NB: Code reads the results as the function's type says they are, and they must fit where they go.
        if !values_match(&values, &self.function(address).ty.results) {
            return Err(Trap::Host(
                "a host function returned results that do not match its type".to_string(),
            ));
        }
        Ok(values)
    }

    /// Calls the host function at `address` with the arguments in `arguments`, its parameter slots in operand stack
    /// order (the first argument in the highest slots), and writes its result slots in that order to `results`.
    ///
    /// # Safety
    ///
    /// `arguments` must have the function's parameter slots and `results` room for its result slots. They may
    /// overlap: every argument is read before a result is written.
    pub(crate) unsafe fn call_host_slots(
        &mut self,
        address: FunctionAddress,
        arguments: *const u64,
        results: *mut u64,
    ) -> Result<(), Trap> {
        // NB: The function lives in a cell of the heap, not in the store, which the call may change.
        let function = unsafe { address.get() };
        let parameter_slots = function.header.param_slots as usize;
        let slots = unsafe { ak::slice_from_raw_parts(arguments, parameter_slots) };
        let mut values = Vec::with_capacity(function.ty.params.len());
        let mut slot = parameter_slots;
        for &ty in &function.ty.params {
            slot -= ty.slot_count() as usize;
            values.push(Value::from_slots(ty, &slots[slot..]));
        }
        let values = self.call_host(address, &values)?;
        let mut next = results;
        for value in values.iter().rev() {
            let (value, count) = value.to_slots();
            unsafe {
                next.copy_from_nonoverlapping(value.as_ptr(), count);
                next = next.add(count);
            }
        }
        Ok(())
    }
}

/// Point the instance's interpreter-visible data at its current contents. The interpreter's view of each entity is at
/// its handle.
fn refresh_instance_data(instance: &mut Instance) {
    // NB: A handle is the address of its entity's interpreter view (see entity.rs), so the instance's vectors of
    //     handles are the arrays of views the interpreter indexes.
    const {
        assert!(size_of::<FunctionAddress>() == size_of::<*const FunctionHeader>());
        assert!(core::mem::offset_of!(FunctionInstance, header) == 0);
        assert!(core::mem::offset_of!(GlobalInstance, value) == 0);
        assert!(core::mem::offset_of!(TableInstance, data) == 0);
        assert!(core::mem::offset_of!(memory::MemoryInstance, data) == 0);
    };
    let data = &mut instance.data;
    data.functions = instance.functions.as_ptr().cast();
    data.function_count = instance.functions.len() as u32;
    data.globals = instance.globals.as_ptr().cast();
    data.tables = instance.tables.as_ptr().cast();
    data.memories = instance.memories.as_ptr().cast();
    data.memory_count = instance.memories.len() as u32;
    data.memory0_base = instance
        .memories
        .first()
        .map_or(std::ptr::null_mut(), |address| unsafe { address.get() }.data.base);
    data.type_identities = instance.type_identities.as_ptr();
    data.tags = instance.tags.as_ptr().cast();
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
