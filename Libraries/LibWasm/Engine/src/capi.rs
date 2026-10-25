/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI the embedders use: compiling modules, and a store to instantiate them in, call their functions, and
//! access their memories, tables, and globals.
//!
//! Modules are reference counted and immutable, so they can be shared across threads. A store and everything in it
//! belongs to one thread. Entities (functions, tables, memories, globals, tags, and instances) are the addresses of
//! their cells.

use crate::module::{ExternalKind, ImportDescription, Module};
use crate::runtime::entity::{Entity, Handle};
use crate::runtime::heap::{GcInterface, Visitor};
use crate::runtime::{ExternValue, FunctionKind, HostFunction, InstantiationError, MemoryAddress, Store, Trap, Value};
use crate::types::{AddressType, FuncType, GlobalType, HeapType, Limits, MemoryType, RefType, TableType, ValType};
use core::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

/// An owned string, which the embedder releases with `wasm_string_release()`.
#[repr(C)]
pub struct WasmString {
    pub data: *mut u8,
    pub length: usize,
}

/// A string borrowed from a module or store, valid while it is.
#[repr(C)]
pub struct WasmStringView {
    pub data: *const u8,
    pub length: usize,
}

impl WasmStringView {
    fn of(text: &str) -> Self {
        Self {
            data: text.as_ptr(),
            length: text.len(),
        }
    }
}

fn write_string(destination: *mut WasmString, text: &str) {
    if destination.is_null() {
        return;
    }
    let bytes: Box<[u8]> = text.as_bytes().into();
    let length = bytes.len();
    unsafe {
        *destination = WasmString {
            data: Box::into_raw(bytes).cast(),
            length,
        };
    }
}

/// Create a string the engine takes ownership of, as a host function's error message.
///
/// # Safety
///
/// `data` must point at `length` readable bytes, and `string` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_string_create(data: *const u8, length: usize, string: *mut WasmString) {
    let text = String::from_utf8_lossy(unsafe { ak::slice_from_raw_parts(data, length) }).into_owned();
    write_string(string, &text);
}

/// Release a string the engine returned.
///
/// # Safety
///
/// The string must come from the engine and not have been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_string_release(string: WasmString) {
    if string.data.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
            string.data,
            string.length,
        )));
    }
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WasmValueKind {
    I32,
    I64,
    F32,
    F64,
    V128,
    Reference,
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WasmHeapKind {
    Function,
    NoFunction,
    Extern,
    NoExtern,
    Any,
    Eq,
    I31,
    Struct,
    Array,
    None,
    Exception,
    NoException,
    /// A type the module defines; `type_index` is its index in the module.
    Concrete,
}

/// A value type. `heap`, `nullable`, and `type_index` only matter for references.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmValueType {
    pub kind: WasmValueKind,
    pub heap: WasmHeapKind,
    pub nullable: bool,
    pub type_index: u32,
}

impl WasmValueType {
    fn of(ty: ValType) -> Self {
        let scalar = |kind| Self {
            kind,
            heap: WasmHeapKind::Function,
            nullable: false,
            type_index: 0,
        };
        match ty {
            ValType::I32 => scalar(WasmValueKind::I32),
            ValType::I64 => scalar(WasmValueKind::I64),
            ValType::F32 => scalar(WasmValueKind::F32),
            ValType::F64 => scalar(WasmValueKind::F64),
            ValType::V128 => scalar(WasmValueKind::V128),
            ValType::Ref(reference) => {
                let (heap, type_index) = match reference.heap {
                    HeapType::Func => (WasmHeapKind::Function, 0),
                    HeapType::NoFunc => (WasmHeapKind::NoFunction, 0),
                    HeapType::Extern => (WasmHeapKind::Extern, 0),
                    HeapType::NoExtern => (WasmHeapKind::NoExtern, 0),
                    HeapType::Any => (WasmHeapKind::Any, 0),
                    HeapType::Eq => (WasmHeapKind::Eq, 0),
                    HeapType::I31 => (WasmHeapKind::I31, 0),
                    HeapType::Struct => (WasmHeapKind::Struct, 0),
                    HeapType::Array => (WasmHeapKind::Array, 0),
                    HeapType::None | HeapType::Bottom => (WasmHeapKind::None, 0),
                    HeapType::Exn => (WasmHeapKind::Exception, 0),
                    HeapType::NoExn => (WasmHeapKind::NoException, 0),
                    HeapType::Concrete(index) => (WasmHeapKind::Concrete, index),
                };
                Self {
                    kind: WasmValueKind::Reference,
                    heap,
                    nullable: reference.nullable,
                    type_index,
                }
            }
        }
    }

    fn to_value_type(self) -> ValType {
        match self.kind {
            WasmValueKind::I32 => ValType::I32,
            WasmValueKind::I64 => ValType::I64,
            WasmValueKind::F32 => ValType::F32,
            WasmValueKind::F64 => ValType::F64,
            WasmValueKind::V128 => ValType::V128,
            WasmValueKind::Reference => ValType::Ref(RefType {
                nullable: self.nullable,
                heap: match self.heap {
                    WasmHeapKind::Function => HeapType::Func,
                    WasmHeapKind::NoFunction => HeapType::NoFunc,
                    WasmHeapKind::Extern => HeapType::Extern,
                    WasmHeapKind::NoExtern => HeapType::NoExtern,
                    WasmHeapKind::Any => HeapType::Any,
                    WasmHeapKind::Eq => HeapType::Eq,
                    WasmHeapKind::I31 => HeapType::I31,
                    WasmHeapKind::Struct => HeapType::Struct,
                    WasmHeapKind::Array => HeapType::Array,
                    WasmHeapKind::None => HeapType::None,
                    WasmHeapKind::Exception => HeapType::Exn,
                    WasmHeapKind::NoException => HeapType::NoExn,
                    WasmHeapKind::Concrete => HeapType::Concrete(self.type_index),
                },
            }),
        }
    }
}

/// A value: i32 and f32 bits in the low 32 bits of `low`, v128 in `low` and `high`, references as their bits (zero
/// for null).
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmValue {
    pub kind: WasmValueKind,
    pub low: u64,
    pub high: u64,
}

impl WasmValue {
    fn of(value: Value) -> Self {
        let (kind, low, high) = match value {
            Value::I32(value) => (WasmValueKind::I32, u64::from(value as u32), 0),
            Value::I64(value) => (WasmValueKind::I64, value as u64, 0),
            Value::F32(bits) => (WasmValueKind::F32, u64::from(bits), 0),
            Value::F64(bits) => (WasmValueKind::F64, bits, 0),
            Value::V128(bits) => (WasmValueKind::V128, bits as u64, (bits >> 64) as u64),
            Value::Ref(bits) => (WasmValueKind::Reference, bits, 0),
        };
        Self { kind, low, high }
    }

    fn to_value(self) -> Value {
        match self.kind {
            WasmValueKind::I32 => Value::I32(self.low as u32 as i32),
            WasmValueKind::I64 => Value::I64(self.low as i64),
            WasmValueKind::F32 => Value::F32(self.low as u32),
            WasmValueKind::F64 => Value::F64(self.low),
            WasmValueKind::V128 => Value::V128(u128::from(self.low) | (u128::from(self.high) << 64)),
            WasmValueKind::Reference => Value::Ref(self.low),
        }
    }
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WasmExternKind {
    Function,
    Table,
    Memory,
    Global,
    Tag,
}

impl WasmExternKind {
    fn of(kind: ExternalKind) -> Self {
        match kind {
            ExternalKind::Function => Self::Function,
            ExternalKind::Table => Self::Table,
            ExternalKind::Memory => Self::Memory,
            ExternalKind::Global => Self::Global,
            ExternalKind::Tag => Self::Tag,
        }
    }
}

/// A function, table, memory, global, or tag of a store.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmExtern {
    pub kind: WasmExternKind,
    /// The entity's cell.
    pub entity: *mut c_void,
}

impl WasmExtern {
    fn of(value: ExternValue) -> Self {
        let (kind, entity) = match value {
            ExternValue::Function(address) => (WasmExternKind::Function, address.cell().cast()),
            ExternValue::Table(address) => (WasmExternKind::Table, address.cell().cast()),
            ExternValue::Memory(address) => (WasmExternKind::Memory, address.cell().cast()),
            ExternValue::Global(address) => (WasmExternKind::Global, address.cell().cast()),
            ExternValue::Tag(address) => (WasmExternKind::Tag, address.cell().cast()),
        };
        Self { kind, entity }
    }

    fn to_extern_value(self) -> ExternValue {
        match self.kind {
            WasmExternKind::Function => ExternValue::Function(entity(self.entity)),
            WasmExternKind::Table => ExternValue::Table(entity(self.entity)),
            WasmExternKind::Memory => ExternValue::Memory(entity(self.entity)),
            WasmExternKind::Global => ExternValue::Global(entity(self.entity)),
            WasmExternKind::Tag => ExternValue::Tag(entity(self.entity)),
        }
    }
}

/// The limits of a memory or table. `maximum` only matters if `has_maximum` is set.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmLimits {
    pub minimum: u64,
    pub maximum: u64,
    pub has_maximum: bool,
    pub address64: bool,
    pub shared: bool,
}

impl WasmLimits {
    fn of(limits: &Limits) -> Self {
        Self {
            minimum: limits.min,
            maximum: limits.max.unwrap_or(0),
            has_maximum: limits.max.is_some(),
            address64: limits.address_type == AddressType::I64,
            shared: limits.shared,
        }
    }

    fn to_limits(self) -> Limits {
        Limits {
            min: self.minimum,
            max: self.has_maximum.then_some(self.maximum),
            address_type: if self.address64 {
                AddressType::I64
            } else {
                AddressType::I32
            },
            shared: self.shared,
        }
    }
}

/// What an import or export is, beyond its kind: the type index of a function or tag, the limits of a memory or
/// table, the element type of a table, and the value type and mutability of a global.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmExternType {
    pub kind: WasmExternKind,
    pub type_index: u32,
    pub limits: WasmLimits,
    pub value_type: WasmValueType,
    pub mutable: bool,
}

fn extern_type_of_import(description: &ImportDescription) -> WasmExternType {
    let mut ty = WasmExternType {
        kind: WasmExternKind::Function,
        type_index: 0,
        limits: WasmLimits::of(&Limits {
            min: 0,
            max: None,
            address_type: AddressType::I32,
            shared: false,
        }),
        value_type: WasmValueType::of(ValType::I32),
        mutable: false,
    };
    match description {
        ImportDescription::Function(index) => ty.type_index = *index,
        ImportDescription::Table(table) => {
            ty.kind = WasmExternKind::Table;
            ty.limits = WasmLimits::of(&table.limits);
            ty.value_type = WasmValueType::of(ValType::Ref(table.element));
        }
        ImportDescription::Memory(memory) => {
            ty.kind = WasmExternKind::Memory;
            ty.limits = WasmLimits::of(&memory.limits);
        }
        ImportDescription::Global(global) => {
            ty.kind = WasmExternKind::Global;
            ty.value_type = WasmValueType::of(global.value);
            ty.mutable = global.mutable;
        }
        ImportDescription::Tag(tag) => {
            ty.kind = WasmExternKind::Tag;
            ty.type_index = tag.type_index;
        }
    }
    ty
}

// Modules.

/// A compiled module, which the embedder holds a reference to until it calls `wasm_module_release()`. Its pointer is
/// that of an `Arc<Module>`, and it is never dereferenced as itself.
pub struct WasmModule {
    _opaque: [u8; 0],
}

/// Decode and validate a module. Returns null and sets `error` if the module is malformed or invalid.
///
/// # Safety
///
/// `bytes` must point at `length` readable bytes, and `error` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_compile(
    bytes: *const u8,
    length: usize,
    error: *mut WasmString,
) -> *const WasmModule {
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
    match crate::compile(bytes) {
        Ok(module) => Arc::into_raw(Arc::new(module)).cast(),
        Err(compile_error) => {
            write_string(error, &compile_error.to_string());
            std::ptr::null()
        }
    }
}

/// # Safety
///
/// `module` must be a live module, which this releases one reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_release(module: *const WasmModule) {
    unsafe { Arc::decrement_strong_count(module.cast::<Module>()) };
}

fn module_of<'a>(module: *const WasmModule) -> &'a Module {
    unsafe { &*module.cast::<Module>() }
}

/// A reference of our own to the module of `module`.
fn module_arc(module: *const WasmModule) -> Arc<Module> {
    let module = module.cast::<Module>();
    unsafe {
        Arc::increment_strong_count(module);
        Arc::from_raw(module)
    }
}

/// # Safety
///
/// `module` must be a live module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_import_count(module: *const WasmModule) -> usize {
    module_of(module).imports.len()
}

/// The names and type of an import.
///
/// # Safety
///
/// `module` must be a live module with an import at `index`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_import(
    module: *const WasmModule,
    index: usize,
    module_name: *mut WasmStringView,
    name: *mut WasmStringView,
    ty: *mut WasmExternType,
) {
    let import = &module_of(module).imports[index];
    unsafe {
        *module_name = WasmStringView::of(&import.module);
        *name = WasmStringView::of(&import.name);
        *ty = extern_type_of_import(&import.description);
    }
}

/// # Safety
///
/// `module` must be a live module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_export_count(module: *const WasmModule) -> usize {
    module_of(module).exports.len()
}

/// The name and type of an export.
///
/// # Safety
///
/// `module` must be a live module with an export at `index`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_export(
    module: *const WasmModule,
    index: usize,
    name: *mut WasmStringView,
    ty: *mut WasmExternType,
) {
    let module = module_of(module);
    let export = &module.exports[index];
    let description = match export.kind {
        ExternalKind::Function => ImportDescription::Function(module.function_space[export.index as usize]),
        ExternalKind::Table => ImportDescription::Table(module.table_space[export.index as usize]),
        ExternalKind::Memory => ImportDescription::Memory(module.memory_space[export.index as usize]),
        ExternalKind::Global => ImportDescription::Global(module.global_space[export.index as usize]),
        ExternalKind::Tag => ImportDescription::Tag(module.tag_space[export.index as usize]),
    };
    unsafe {
        *name = WasmStringView::of(&export.name);
        *ty = extern_type_of_import(&description);
        (*ty).kind = WasmExternKind::of(export.kind);
    }
}

/// The parameter and result counts of the function type at `type_index`, or false if it is not a function type.
///
/// # Safety
///
/// `module` must be a live module, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_function_type(
    module: *const WasmModule,
    type_index: u32,
    parameter_count: *mut usize,
    result_count: *mut usize,
) -> bool {
    let Some(function) = module_of(module)
        .types
        .get(type_index as usize)
        .and_then(|ty| ty.as_func())
    else {
        return false;
    };
    unsafe {
        *parameter_count = function.params.len();
        *result_count = function.results.len();
    }
    true
}

/// Copy the parameter and result types of the function type at `type_index`.
///
/// # Safety
///
/// `module` must be a live module with a function type at `type_index`, and the out arrays must have room for its
/// parameters and results.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_function_type_values(
    module: *const WasmModule,
    type_index: u32,
    parameters: *mut WasmValueType,
    results: *mut WasmValueType,
) {
    let function = module_of(module).types[type_index as usize]
        .as_func()
        .expect("the embedder checks for a function type");
    unsafe { write_function_type(function, parameters, results) };
}

/// Write the parameter and result types of `function`; a null `results` skips the results.
unsafe fn write_function_type(function: &FuncType, parameters: *mut WasmValueType, results: *mut WasmValueType) {
    for (index, &ty) in function.params.iter().enumerate() {
        unsafe { *parameters.add(index) = WasmValueType::of(ty) };
    }
    if results.is_null() {
        return;
    }
    for (index, &ty) in function.results.iter().enumerate() {
        unsafe { *results.add(index) = WasmValueType::of(ty) };
    }
}

// Stores.

/// A store, which belongs to the thread that created it.
pub struct WasmStore(Store);

/// Create a store for the current thread, whose entities, GC objects, and memories live in the LibGC heap and
/// primitive storage of `interface`. The store keeps no entity alive: its embedder visits or roots the cells of those
/// it uses (see `WasmExtern`).
///
/// # Safety
///
/// `interface` must hold the functions of LibGC's C interface and a heap of the current thread, which outlives the
/// store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_create(interface: *const GcInterface) -> *mut WasmStore {
    let interface = unsafe { *interface };
    let store = Box::into_raw(Box::new(WasmStore(Store::with_heap(Some(interface)))));
    unsafe { (*store).0.attach_to_heap() };
    store
}

/// # Safety
///
/// `store` must be a live store that no call is running in.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_destroy(store: *mut WasmStore) {
    unsafe { drop(Box::from_raw(store)) };
}

/// The entity of a cell an embedder passed in. Panics if the cell is not a live entity of the kind.
fn entity<T: Entity>(cell: *mut c_void) -> Handle<T> {
    unsafe { Handle::from_cell(cell) }.expect("the embedder passes a live entity of the right kind")
}

fn store_of<'a>(store: *mut WasmStore) -> &'a mut Store {
    unsafe { &mut (*store).0 }
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WasmStatus {
    Ok,
    /// Linking failed: an import is missing or has the wrong type.
    LinkError,
    /// Execution trapped.
    Trap,
    /// An exception was thrown and not caught.
    Exception,
    /// Execution ran out of stack.
    StackExhausted,
}

impl WasmStatus {
    /// The status of a trap, with what the embedder needs to know of it in `error` and `exception`.
    fn of_trap(trap: &Trap, error: *mut WasmString, exception: *mut u64) -> Self {
        match trap {
            Trap::UncaughtException(reference) => {
                if !exception.is_null() {
                    unsafe { *exception = *reference };
                }
                write_string(error, &trap.message());
                Self::Exception
            }
            Trap::StackExhausted => {
                write_string(error, &trap.message());
                Self::StackExhausted
            }
            _ => {
                write_string(error, &trap.message());
                Self::Trap
            }
        }
    }
}

/// A function the embedder implements. It reads the arguments and writes the results, in the order of the function
/// type, and returns `Ok`. It traps by returning `Trap` with a message in `error`, and throws an exception by
/// returning `Exception` with the exception's reference in `exception`.
pub type WasmHostCallback = unsafe extern "C" fn(
    user_data: *mut std::ffi::c_void,
    store: *mut WasmStore,
    arguments: *const WasmValue,
    argument_count: usize,
    results: *mut WasmValue,
    result_count: usize,
    error: *mut WasmString,
    exception: *mut u64,
) -> WasmStatus;

/// Visits the cells the user data of a host function refers to, with a LibGC visitor.
pub type WasmVisitCallback = unsafe extern "C" fn(user_data: *mut std::ffi::c_void, visitor: *mut std::ffi::c_void);

/// Releases the user data of a host function when its function goes away.
pub type WasmReleaseCallback = unsafe extern "C" fn(user_data: *mut std::ffi::c_void);

struct HostFunctionData {
    callback: WasmHostCallback,
    visit: WasmVisitCallback,
    release: WasmReleaseCallback,
    user_data: *mut std::ffi::c_void,
    /// The store the function belongs to, which its calls get.
    store: *mut WasmStore,
    result_count: usize,
}

impl Drop for HostFunctionData {
    fn drop(&mut self) {
        unsafe { (self.release)(self.user_data) };
    }
}

impl HostFunction for HostFunctionData {
    fn call(&self, _: &mut Store, arguments: &[Value]) -> Result<Vec<Value>, Trap> {
        let arguments: Vec<WasmValue> = arguments.iter().map(|&value| WasmValue::of(value)).collect();
        let mut results = vec![
            WasmValue {
                kind: WasmValueKind::I32,
                low: 0,
                high: 0,
            };
            self.result_count
        ];
        let mut error = WasmString {
            data: std::ptr::null_mut(),
            length: 0,
        };
        let mut exception = 0u64;
        let status = unsafe {
            (self.callback)(
                self.user_data,
                self.store,
                arguments.as_ptr(),
                arguments.len(),
                results.as_mut_ptr(),
                results.len(),
                &raw mut error,
                &raw mut exception,
            )
        };
        let message = if error.data.is_null() {
            String::new()
        } else {
            let bytes = unsafe { ak::slice_from_raw_parts(error.data, error.length) };
            let message = String::from_utf8_lossy(bytes).into_owned();
            unsafe { wasm_string_release(error) };
            message
        };
        match status {
            WasmStatus::Ok => Ok(results.into_iter().map(WasmValue::to_value).collect()),
            WasmStatus::Exception => Err(Trap::UncaughtException(exception)),
            _ => Err(Trap::Host(message)),
        }
    }

    fn visit_edges(&self, visitor: &Visitor) {
        unsafe { (self.visit)(self.user_data, visitor.raw()) };
    }

    fn user_data(&self) -> *mut c_void {
        self.user_data
    }
}

/// Add a host function of the given type to the store, and return its cell. The function visits what its user data
/// refers to with `visit`, and releases the user data with `release` when it goes away.
///
/// # Safety
///
/// `store` must be a live store, and the type arrays must hold the given numbers of types.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_host_function(
    store: *mut WasmStore,
    parameters: *const WasmValueType,
    parameter_count: usize,
    results: *const WasmValueType,
    result_count: usize,
    callback: WasmHostCallback,
    visit: WasmVisitCallback,
    release: WasmReleaseCallback,
    user_data: *mut std::ffi::c_void,
) -> *mut c_void {
    let to_types = |types: *const WasmValueType, count: usize| -> Vec<ValType> {
        unsafe { ak::slice_from_raw_parts(types, count) }
            .iter()
            .map(|ty| ty.to_value_type())
            .collect()
    };
    let ty = FuncType {
        params: to_types(parameters, parameter_count),
        results: to_types(results, result_count),
    };
    let data = Rc::new(HostFunctionData {
        callback,
        visit,
        release,
        user_data,
        store,
        result_count: ty.results.len(),
    });
    store_of(store).allocate_host_function(ty, data).cell().cast()
}

/// Add a memory to the store. Returns false if it cannot be allocated.
///
/// # Safety
///
/// `store` must be a live store, and `address` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_memory(
    store: *mut WasmStore,
    limits: WasmLimits,
    address: *mut *mut c_void,
) -> bool {
    let Some(allocated) = store_of(store).allocate_memory(MemoryType {
        limits: limits.to_limits(),
    }) else {
        return false;
    };
    unsafe { *address = allocated.cell().cast() };
    true
}

/// Add a table to the store, with every element set to `initial`. Returns false if it would be too large.
///
/// # Safety
///
/// `store` must be a live store, and `address` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_table(
    store: *mut WasmStore,
    element: WasmValueType,
    limits: WasmLimits,
    initial: u64,
    address: *mut *mut c_void,
) -> bool {
    let ValType::Ref(element) = element.to_value_type() else {
        panic!("table elements are references");
    };
    let Some(allocated) = store_of(store).allocate_table(
        TableType {
            element,
            limits: limits.to_limits(),
        },
        initial,
    ) else {
        return false;
    };
    unsafe { *address = allocated.cell().cast() };
    true
}

/// Add a global to the store.
///
/// # Safety
///
/// `store` must be a live store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_global(
    store: *mut WasmStore,
    ty: WasmValueType,
    mutable: bool,
    value: WasmValue,
) -> *mut c_void {
    store_of(store)
        .allocate_global(
            GlobalType {
                value: ty.to_value_type(),
                mutable,
            },
            value.to_value(),
        )
        .cell()
        .cast()
}

/// Instantiate `module` with `imports`, one for each of its imports in order. On success, writes the instance's
/// address; otherwise writes a message to `error`.
///
/// # Safety
///
/// `store` and `module` must be live, `imports` must hold `import_count` externs, and the out parameters must be
/// writable (`error` and `exception` may be null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instantiate(
    store: *mut WasmStore,
    module: *const WasmModule,
    imports: *const WasmExtern,
    import_count: usize,
    instance: *mut *mut c_void,
    error: *mut WasmString,
    exception: *mut u64,
) -> WasmStatus {
    let mut values = Vec::with_capacity(import_count);
    for import in unsafe { ak::slice_from_raw_parts(imports, import_count) } {
        values.push(import.to_extern_value());
    }
    let module = module_arc(module);
    match store_of(store).instantiate(module, &values) {
        Ok(address) => {
            unsafe { *instance = address.cell().cast() };
            WasmStatus::Ok
        }
        Err(InstantiationError::Link(message)) => {
            write_string(error, &message);
            WasmStatus::LinkError
        }
        Err(InstantiationError::Trap(trap)) => WasmStatus::of_trap(&trap, error, exception),
    }
}

/// The number of exports of an instance.
///
/// # Safety
///
/// `store` must be a live store with an instance at `instance`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instance_export_count(store: *mut WasmStore, instance: *mut c_void) -> usize {
    store_of(store).instance(entity(instance)).module.exports.len()
}

/// The name and value of an instance's export, in the module's order.
///
/// # Safety
///
/// `store` must be a live store with an instance at `instance` that has an export at `index`, and the out parameters
/// must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instance_export(
    store: *mut WasmStore,
    instance: *mut c_void,
    index: usize,
    name: *mut WasmStringView,
    value: *mut WasmExtern,
) -> bool {
    let instance = store_of(store).instance(entity(instance));
    let export = &instance.module.exports[index];
    unsafe { *name = WasmStringView::of(&export.name) };
    let Some(extern_value) = instance.export(&export.name) else {
        return false;
    };
    unsafe { *value = WasmExtern::of(extern_value) };
    true
}

/// Find an instance's export by name.
///
/// # Safety
///
/// `store` must be a live store with an instance at `instance`, `name` must hold `length` bytes, and `value` must be
/// writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instance_find_export(
    store: *mut WasmStore,
    instance: *mut c_void,
    name: *const u8,
    length: usize,
    value: *mut WasmExtern,
) -> bool {
    let name = unsafe { ak::slice_from_raw_parts(name, length) };
    let Ok(name) = std::str::from_utf8(name) else {
        return false;
    };
    let Some(extern_value) = store_of(store).instance(entity(instance)).export(name) else {
        return false;
    };
    unsafe { *value = WasmExtern::of(extern_value) };
    true
}

/// The parameter and result counts of a function.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_type(
    store: *mut WasmStore,
    function: *mut c_void,
    parameter_count: *mut usize,
    result_count: *mut usize,
) {
    let ty = &store_of(store).function(entity(function)).ty;
    unsafe {
        *parameter_count = ty.params.len();
        *result_count = ty.results.len();
    }
}

/// Copy the parameter and result types of a function.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`, and the out arrays must have room for its parameters
/// and results.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_type_values(
    store: *mut WasmStore,
    function: *mut c_void,
    parameters: *mut WasmValueType,
    results: *mut WasmValueType,
) {
    let ty = &store_of(store).function(entity(function)).ty;
    unsafe { write_function_type(ty, parameters, results) };
}

/// Call a function. On a trap, writes its message to `error`.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`, `arguments` must hold one value for each parameter,
/// `results` must have room for one value for each result, and `error` and `exception` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_invoke(
    store: *mut WasmStore,
    function: *mut c_void,
    arguments: *const WasmValue,
    argument_count: usize,
    results: *mut WasmValue,
    result_count: usize,
    error: *mut WasmString,
    exception: *mut u64,
) -> WasmStatus {
    let arguments: Vec<Value> = unsafe { ak::slice_from_raw_parts(arguments, argument_count) }
        .iter()
        .map(|value| value.to_value())
        .collect();
    match store_of(store).invoke(entity(function), &arguments) {
        Ok(values) => {
            for (index, value) in values.into_iter().take(result_count).enumerate() {
                unsafe { *results.add(index) = WasmValue::of(value) };
            }
            WasmStatus::Ok
        }
        Err(trap) => WasmStatus::of_trap(&trap, error, exception),
    }
}

/// The reference to a function, as tables and reference values hold it.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_reference(store: *mut WasmStore, function: *mut c_void) -> u64 {
    store_of(store).function_reference(entity(function))
}

/// The function a function reference refers to. Returns false for null.
///
/// # Safety
///
/// `store` must be a live store, `reference` a function reference of it, and `function` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_from_reference(
    store: *mut WasmStore,
    reference: u64,
    function: *mut *mut c_void,
) -> bool {
    let Some(address) = store_of(store).function_from_reference(reference) else {
        return false;
    };
    unsafe { *function = address.cell().cast() };
    true
}

/// The bytes of a memory, which move when it grows.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`, and `size` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_data(
    store: *mut WasmStore,
    memory: *mut c_void,
    size: *mut usize,
) -> *mut u8 {
    let memory = store_of(store).memory_mut(entity(memory));
    unsafe { *size = memory.size() };
    memory.bytes_mut().as_mut_ptr()
}

/// Grow a memory by `delta` pages. Returns the previous size in pages, or -1 if it cannot grow.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_grow(store: *mut WasmStore, memory: *mut c_void, delta: u64) -> i64 {
    store_of(store)
        .memory_mut(entity(memory))
        .grow(delta)
        .map_or(-1, |previous| previous as i64)
}

/// The limits a memory was created with.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_limits(store: *mut WasmStore, memory: *mut c_void) -> WasmLimits {
    WasmLimits::of(&store_of(store).memory(entity(memory)).ty.limits)
}

/// # Safety
///
/// `store` must be a live store with a table at `table`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_size(store: *mut WasmStore, table: *mut c_void) -> u64 {
    store_of(store).table(entity(table)).elements.len() as u64
}

/// The element type and limits a table was created with.
///
/// # Safety
///
/// `store` must be a live store with a table at `table`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_type(
    store: *mut WasmStore,
    table: *mut c_void,
    element: *mut WasmValueType,
    limits: *mut WasmLimits,
) {
    let table = store_of(store).table(entity(table));
    unsafe {
        *element = WasmValueType::of(ValType::Ref(table.ty.element));
        *limits = WasmLimits::of(&table.ty.limits);
    }
}

/// Read a table element. Returns false if `index` is out of bounds.
///
/// # Safety
///
/// `store` must be a live store with a table at `table`, and `value` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_get(
    store: *mut WasmStore,
    table: *mut c_void,
    index: u64,
    value: *mut u64,
) -> bool {
    let Some(&element) = store_of(store).table(entity(table)).elements.get(index as usize) else {
        return false;
    };
    unsafe { *value = element };
    true
}

/// Write a table element. Returns false if `index` is out of bounds.
///
/// # Safety
///
/// `store` must be a live store with a table at `table`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_set(
    store: *mut WasmStore,
    table: *mut c_void,
    index: u64,
    value: u64,
) -> bool {
    let Some(element) = store_of(store)
        .table_mut(entity(table))
        .elements
        .get_mut(index as usize)
    else {
        return false;
    };
    *element = value;
    true
}

/// Grow a table by `delta` elements set to `initial`. Returns the previous size, or -1 if it cannot grow.
///
/// # Safety
///
/// `store` must be a live store with a table at `table`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_grow(
    store: *mut WasmStore,
    table: *mut c_void,
    delta: u64,
    initial: u64,
) -> i64 {
    store_of(store)
        .table_mut(entity(table))
        .grow(delta, initial)
        .map_or(-1, |previous| previous as i64)
}

/// The type of a global.
///
/// # Safety
///
/// `store` must be a live store with a global at `global`, and `mutable` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_global_type(
    store: *mut WasmStore,
    global: *mut c_void,
    mutable: *mut bool,
) -> WasmValueType {
    let ty = store_of(store).global(entity(global)).ty;
    unsafe { *mutable = ty.mutable };
    WasmValueType::of(ty.value)
}

/// # Safety
///
/// `store` must be a live store with a global at `global`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_global_get(store: *mut WasmStore, global: *mut c_void) -> WasmValue {
    WasmValue::of(store_of(store).global(entity(global)).get())
}

/// # Safety
///
/// `store` must be a live store with a global at `global`, and `value` must have the global's type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_global_set(store: *mut WasmStore, global: *mut c_void, value: WasmValue) {
    store_of(store).global_mut(entity(global)).set(value.to_value());
}

// Arrays.

/// The number of elements of an array.
///
/// # Safety
///
/// `array` must be a live array reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_array_length(array: u64) -> u32 {
    unsafe { crate::runtime::gc::array_length(array) }
}

/// The bits of the element at `index` of an array whose elements are numbers, with packed elements zero-extended.
///
/// # Safety
///
/// `array` must be a live array reference whose elements are neither references nor v128s, and `index` must be in
/// its bounds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_array_element(array: u64, index: u32) -> u64 {
    unsafe { crate::runtime::gc::array_element(array, index) }
}

/// Set the element at `index` of an array whose elements are numbers to `bits`, which packed elements wrap.
///
/// # Safety
///
/// `array` must be a live array reference whose elements are neither references nor v128s, and `index` must be in
/// its bounds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_array_set_element(array: u64, index: u32, bits: u64) {
    unsafe { crate::runtime::gc::set_array_element(array, index, bits) }
}

// Custom sections.

/// The number of custom sections of a module.
///
/// # Safety
///
/// `module` must be a live module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_custom_section_count(module: *const WasmModule) -> usize {
    module_of(module).custom_sections.len()
}

/// The name and contents of a module's custom section, in the order of the binary.
///
/// # Safety
///
/// `module` must be a live module with a custom section at `index`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_custom_section(
    module: *const WasmModule,
    index: usize,
    name: *mut WasmStringView,
    contents: *mut WasmStringView,
) {
    let module = module_of(module);
    let section = &module.custom_sections[index];
    let bytes = &module.bytes[section.contents.start as usize..section.contents.end as usize];
    unsafe {
        *name = WasmStringView::of(&section.name);
        *contents = WasmStringView {
            data: bytes.as_ptr(),
            length: bytes.len(),
        };
    }
}

// Tags and exceptions.

/// Add a tag for exceptions that carry values of the given types, and return its address.
///
/// # Safety
///
/// `store` must be a live store, and `parameters` must hold `parameter_count` types.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_tag(
    store: *mut WasmStore,
    parameters: *const WasmValueType,
    parameter_count: usize,
) -> *mut c_void {
    let params = unsafe { ak::slice_from_raw_parts(parameters, parameter_count) }
        .iter()
        .map(|ty| ty.to_value_type())
        .collect();
    store_of(store)
        .allocate_tag(FuncType {
            params,
            results: Vec::new(),
        })
        .cell()
        .cast()
}

/// `ty` with its concrete heap type, which `module` defines, replaced by the type's store-wide identity. The types the
/// store reports for functions, tables, globals, and tags have identities in place of type indices too.
///
/// # Safety
///
/// `store` must be a live store and `module` a live module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_canonical_value_type(
    store: *mut WasmStore,
    module: *const WasmModule,
    ty: WasmValueType,
) -> WasmValueType {
    if ty.kind != WasmValueKind::Reference || ty.heap != WasmHeapKind::Concrete {
        return ty;
    }
    let identities = store_of(store).module_type_identities_of(&module_arc(module));
    WasmValueType {
        type_index: identities[ty.type_index as usize],
        ..ty
    }
}

/// Whether a non-null reference matches the reference type `ty`, whose concrete heap type is an identity. A
/// reference of the any hierarchy must be in its internal form: an i31 reference, a GC object, or a host reference.
///
/// # Safety
///
/// `store` must be a live store, and `reference` a live reference of the hierarchy of `ty`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_reference_matches(
    store: *mut WasmStore,
    reference: u64,
    ty: WasmValueType,
) -> bool {
    let ValType::Ref(reference_type) = ty.to_value_type() else {
        return false;
    };
    store_of(store).reference_matches(reference, reference_type.heap)
}

/// Whether the type `identity` is a function type, as opposed to a struct or array type.
///
/// # Safety
///
/// `store` must be a live store with a type `identity`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_is_function_type(store: *mut WasmStore, identity: u32) -> bool {
    store_of(store).is_function_type(identity)
}

/// The number of values an exception with the tag carries.
///
/// # Safety
///
/// `store` must be a live store with a tag at `tag`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_tag_parameter_count(store: *mut WasmStore, tag: *mut c_void) -> usize {
    store_of(store).tag(entity(tag)).ty.params.len()
}

/// The types of the values an exception with the tag carries.
///
/// # Safety
///
/// `store` must be a live store with a tag at `tag`, and `parameters` must have room for its parameter count.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_tag_parameters(
    store: *mut WasmStore,
    tag: *mut c_void,
    parameters: *mut WasmValueType,
) {
    let ty = &store_of(store).tag(entity(tag)).ty;
    unsafe { write_function_type(ty, parameters, std::ptr::null_mut()) };
}

/// Create an exception with the tag, carrying `values`, and return its reference. The exception is a cell of the
/// store's heap, which keeps it while something refers to it.
///
/// # Safety
///
/// `store` must be a live store with a tag at `tag`, and `values` must hold one value of each of its parameter types.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_exception(
    store: *mut WasmStore,
    tag: *mut c_void,
    values: *const WasmValue,
    value_count: usize,
) -> u64 {
    let values: Vec<Value> = unsafe { ak::slice_from_raw_parts(values, value_count) }
        .iter()
        .map(|value| value.to_value())
        .collect();
    store_of(store).allocate_exception_with_values(entity(tag), &values)
}

/// The tag of an exception.
///
/// # Safety
///
/// `store` must be a live store, and `exception` a reference to one of its exceptions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_exception_tag(store: *mut WasmStore, exception: u64) -> *mut c_void {
    store_of(store).exception(exception).tag.cell().cast()
}

/// The values an exception carries.
///
/// # Safety
///
/// `store` must be a live store, `exception` a reference to one of its exceptions, and `values` must have room for
/// the parameter count of its tag.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_exception_values(store: *mut WasmStore, exception: u64, values: *mut WasmValue) {
    for (index, value) in store_of(store).exception_values(exception).into_iter().enumerate() {
        unsafe { *values.add(index) = WasmValue::of(value) };
    }
}

// Functions, memories.

/// The index of a WebAssembly function in the function index space of the module that defines it. Returns false for a
/// host function.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`, and `index` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_index(
    store: *mut WasmStore,
    function: *mut c_void,
    index: *mut u32,
) -> bool {
    let FunctionKind::Wasm {
        index: function_index, ..
    } = store_of(store).function(entity(function)).kind
    else {
        return false;
    };
    unsafe { *index = function_index };
    true
}

/// The user data a host function was allocated with, or null if the function is no host function.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_host_function_user_data(
    store: *mut WasmStore,
    function: *mut c_void,
) -> *mut c_void {
    match &store_of(store).function(entity(function)).kind {
        FunctionKind::Host(callback) => callback.user_data(),
        FunctionKind::Wasm { .. } => core::ptr::null_mut(),
    }
}

/// The handle of the LibGC primitive storage a memory lives in, or zero if the store has no heap.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_primitive_storage_handle(store: *mut WasmStore, memory: *mut c_void) -> u64 {
    store_of(store).memory(entity(memory)).primitive_storage_handle()
}

/// Called with a memory's address after `memory.grow` grows it.
pub type WasmMemoryGrowCallback = unsafe extern "C" fn(user_data: *mut std::ffi::c_void, memory: *mut c_void);

/// Have `callback` run with `user_data` after each `memory.grow` that grows a memory of the store, but not after
/// `wasm_store_memory_grow()`.
///
/// # Safety
///
/// `store` must be a live store, and `user_data` must stay valid until the callback is cleared or the store is
/// destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_set_memory_grow_callback(
    store: *mut WasmStore,
    callback: WasmMemoryGrowCallback,
    user_data: *mut std::ffi::c_void,
) {
    store_of(store).memory_grow_callback = Some(Rc::new(move |memory: MemoryAddress| unsafe {
        callback(user_data, memory.cell().cast());
    }));
}

/// Remove the callback `wasm_store_set_memory_grow_callback()` set.
///
/// # Safety
///
/// `store` must be a live store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_clear_memory_grow_callback(store: *mut WasmStore) {
    store_of(store).memory_grow_callback = None;
}

/// Sets whether the agent the store runs code for may block, which `memory.atomic.wait` needs.
///
/// # Safety
///
/// `store` must be a live store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_set_can_block(store: *mut WasmStore, can_block: bool) {
    store_of(store).can_block = can_block;
}

/// Has `cell`, an entity, or an exception or a GC object (whose references are their cells), keep the embedder's cell
/// `object` alive for as long as it lives itself.
///
/// # Safety
///
/// `cell` must be a live cell of a store with a LibGC heap, and `object` a cell of the same heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_set_host_object(cell: *mut c_void, object: *mut c_void) {
    unsafe { crate::runtime::heap::set_host_object(cell.cast(), object) };
}

/// The cell `wasm_set_host_object()` gave `cell` to keep alive, or null.
///
/// # Safety
///
/// `cell` must be a live cell of a store with a LibGC heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_host_object(cell: *mut c_void) -> *mut c_void {
    unsafe { crate::runtime::heap::host_object(cell) }
}

// WASI.

/// The state of a WASI command, whose functions a store gets with `wasm_wasi_allocate_function()`.
pub struct WasmWasi(Rc<crate::wasi::Wasi>);

fn strings_of(strings: *const WasmStringView, count: usize) -> Vec<String> {
    if count == 0 {
        return Vec::new();
    }
    unsafe { ak::slice_from_raw_parts(strings, count) }
        .iter()
        .map(|string| {
            String::from_utf8_lossy(unsafe { ak::slice_from_raw_parts(string.data, string.length) }).into_owned()
        })
        .collect()
}

/// Create the state of a WASI command with its arguments, its environment (as `NAME=value` strings), and the host
/// directories it sees at the guest paths of the same index.
///
/// # Safety
///
/// Each array must hold its count of strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_wasi_create(
    arguments: *const WasmStringView,
    argument_count: usize,
    environment: *const WasmStringView,
    environment_count: usize,
    host_paths: *const WasmStringView,
    guest_paths: *const WasmStringView,
    preopen_count: usize,
) -> *mut WasmWasi {
    let preopens = strings_of(host_paths, preopen_count)
        .into_iter()
        .zip(strings_of(guest_paths, preopen_count))
        .map(|(host, guest)| crate::wasi::Preopen::new(host, guest))
        .collect();
    let wasi = crate::wasi::Wasi::new(
        strings_of(arguments, argument_count),
        strings_of(environment, environment_count),
        preopens,
    );
    Box::into_raw(Box::new(WasmWasi(Rc::new(wasi))))
}

/// # Safety
///
/// `wasi` must come from `wasm_wasi_create()`. The functions made from it keep what they need.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_wasi_destroy(wasi: *mut WasmWasi) {
    unsafe { drop(Box::from_raw(wasi)) };
}

/// Add the WASI function `name` (of `wasi_snapshot_preview1`) to the store, and write its cell to `function`.
/// Returns false if there is no such function.
///
/// # Safety
///
/// `store` and `wasi` must be live, `name` must hold `length` bytes, and `function` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_wasi_allocate_function(
    store: *mut WasmStore,
    wasi: *mut WasmWasi,
    name: *const u8,
    length: usize,
    function: *mut *mut c_void,
) -> bool {
    let Ok(name) = std::str::from_utf8(unsafe { ak::slice_from_raw_parts(name, length) }) else {
        return false;
    };
    let Some(address) = unsafe { &(*wasi).0 }.allocate_function(store_of(store), name) else {
        return false;
    };
    unsafe { *function = address.cell().cast() };
    true
}

/// Have the WASI functions work on `memory`, which is usually the memory the command exports. The memory must live
/// while they run, as the command's instance makes it.
///
/// # Safety
///
/// `wasi` must be live, and `memory` a memory's cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_wasi_set_memory(wasi: *mut WasmWasi, memory: *mut c_void) {
    unsafe { &(*wasi).0 }.set_memory(entity(memory));
}

/// Whether the command called proc_exit, and with which code.
///
/// # Safety
///
/// `wasi` must be live, and `code` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_wasi_exit_code(wasi: *mut WasmWasi, code: *mut i32) -> bool {
    let Some(exit_code) = unsafe { &(*wasi).0 }.exit_code() else {
        return false;
    };
    unsafe { *code = exit_code };
    true
}
