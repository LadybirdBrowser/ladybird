/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI the embedders use: compiling modules, and a store to instantiate them in, call their functions, and
//! access their memories, tables, and globals.
//!
//! Modules are reference counted and immutable, so they can be shared across threads. A store and everything in it
//! belongs to one thread. Addresses index the store's functions, tables, memories, globals, and instances.

use crate::module::{ExternalKind, ImportDescription, Module};
use crate::runtime::{
    ExternValue, FunctionAddress, GlobalAddress, InstanceAddress, InstantiationError, MemoryAddress, Store,
    TableAddress, TagAddress, Trap, Value,
};
use crate::types::{AddressType, FuncType, GlobalType, HeapType, Limits, MemoryType, RefType, TableType, ValType};
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

/// A function, table, memory, or global of a store.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct WasmExtern {
    pub kind: WasmExternKind,
    pub address: u32,
}

impl WasmExtern {
    fn of(value: ExternValue) -> Self {
        let (kind, address) = match value {
            ExternValue::Function(address) => (WasmExternKind::Function, address.0),
            ExternValue::Table(address) => (WasmExternKind::Table, address.0),
            ExternValue::Memory(address) => (WasmExternKind::Memory, address.0),
            ExternValue::Global(address) => (WasmExternKind::Global, address.0),
            ExternValue::Tag(address) => (WasmExternKind::Tag, address.0),
        };
        Self { kind, address }
    }

    fn to_extern_value(self) -> ExternValue {
        match self.kind {
            WasmExternKind::Function => ExternValue::Function(FunctionAddress(self.address)),
            WasmExternKind::Table => ExternValue::Table(TableAddress(self.address)),
            WasmExternKind::Memory => ExternValue::Memory(MemoryAddress(self.address)),
            WasmExternKind::Global => ExternValue::Global(GlobalAddress(self.address)),
            WasmExternKind::Tag => ExternValue::Tag(TagAddress(self.address)),
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

/// A compiled module. The embedder holds it through `wasm_module_retain()` and `wasm_module_release()`.
pub struct WasmModule(Arc<Module>);

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
        Ok(module) => Arc::into_raw(Arc::new(WasmModule(Arc::new(module)))),
        Err(compile_error) => {
            write_string(error, &compile_error.to_string());
            std::ptr::null()
        }
    }
}

/// # Safety
///
/// `module` must be a live module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_retain(module: *const WasmModule) {
    unsafe { Arc::increment_strong_count(module) };
}

/// # Safety
///
/// `module` must be a live module, which this releases one reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_module_release(module: *const WasmModule) {
    unsafe { Arc::decrement_strong_count(module) };
}

fn module_of<'a>(module: *const WasmModule) -> &'a Module {
    unsafe { &(*module).0 }
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

unsafe fn write_function_type(function: &FuncType, parameters: *mut WasmValueType, results: *mut WasmValueType) {
    for (index, &ty) in function.params.iter().enumerate() {
        unsafe { *parameters.add(index) = WasmValueType::of(ty) };
    }
    for (index, &ty) in function.results.iter().enumerate() {
        unsafe { *results.add(index) = WasmValueType::of(ty) };
    }
}

// Stores.

/// A store, which belongs to the thread that created it.
pub struct WasmStore(Store);

/// Create a store for the current thread.
#[unsafe(no_mangle)]
pub extern "C" fn wasm_store_create() -> *mut WasmStore {
    Box::into_raw(Box::new(WasmStore(Store::new())))
}

/// # Safety
///
/// `store` must be a live store that no call is running in.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_destroy(store: *mut WasmStore) {
    unsafe { drop(Box::from_raw(store)) };
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
}

/// A function the embedder implements. It reads the arguments and writes the results, in the order of the function
/// type, and returns false with a message in `error` to trap.
pub type WasmHostCallback = unsafe extern "C" fn(
    user_data: *mut std::ffi::c_void,
    store: *mut WasmStore,
    arguments: *const WasmValue,
    argument_count: usize,
    results: *mut WasmValue,
    result_count: usize,
    error: *mut WasmString,
) -> bool;

/// Releases the user data of a host function when the store is destroyed.
pub type WasmReleaseCallback = unsafe extern "C" fn(user_data: *mut std::ffi::c_void);

struct HostFunctionData {
    callback: WasmHostCallback,
    user_data: *mut std::ffi::c_void,
    release: WasmReleaseCallback,
}

impl Drop for HostFunctionData {
    fn drop(&mut self) {
        unsafe { (self.release)(self.user_data) };
    }
}

/// Add a host function of the given type to the store, and return its address.
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
    user_data: *mut std::ffi::c_void,
    release: WasmReleaseCallback,
) -> u32 {
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
    let wasm_store = store;
    let data = Rc::new(HostFunctionData {
        callback,
        user_data,
        release,
    });
    let result_count = ty.results.len();
    let address = store_of(store).allocate_host_function(
        ty,
        // The callback gets the store it runs in, which is this one.
        Rc::new(move |_: &mut Store, arguments: &[Value]| {
            let arguments: Vec<WasmValue> = arguments.iter().map(|&value| WasmValue::of(value)).collect();
            let mut results = vec![
                WasmValue {
                    kind: WasmValueKind::I32,
                    low: 0,
                    high: 0,
                };
                result_count
            ];
            let mut error = WasmString {
                data: std::ptr::null_mut(),
                length: 0,
            };
            let succeeded = unsafe {
                (data.callback)(
                    data.user_data,
                    wasm_store,
                    arguments.as_ptr(),
                    arguments.len(),
                    results.as_mut_ptr(),
                    results.len(),
                    &raw mut error,
                )
            };
            if !succeeded {
                let message = if error.data.is_null() {
                    String::new()
                } else {
                    let bytes = unsafe { ak::slice_from_raw_parts(error.data, error.length) };
                    let message = String::from_utf8_lossy(bytes).into_owned();
                    unsafe { wasm_string_release(error) };
                    message
                };
                return Err(Trap::Host(message));
            }
            Ok(results.into_iter().map(WasmValue::to_value).collect())
        }),
    );
    address.0
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
    address: *mut u32,
) -> bool {
    let Some(allocated) = store_of(store).allocate_memory(MemoryType {
        limits: limits.to_limits(),
    }) else {
        return false;
    };
    unsafe { *address = allocated.0 };
    true
}

/// Add a table to the store, with every element set to `initial`.
///
/// # Safety
///
/// `store` must be a live store.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_allocate_table(
    store: *mut WasmStore,
    element: WasmValueType,
    limits: WasmLimits,
    initial: u64,
) -> u32 {
    let ValType::Ref(element) = element.to_value_type() else {
        panic!("table elements are references");
    };
    store_of(store)
        .allocate_table(
            TableType {
                element,
                limits: limits.to_limits(),
            },
            initial,
        )
        .0
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
) -> u32 {
    store_of(store)
        .allocate_global(
            GlobalType {
                value: ty.to_value_type(),
                mutable,
            },
            value.to_value(),
        )
        .0
}

/// Instantiate `module` with `imports`, one for each of its imports in order. On success, writes the instance's
/// address; otherwise writes a message to `error`.
///
/// # Safety
///
/// `store` and `module` must be live, `imports` must hold `import_count` externs, and the out parameters must be
/// writable (`error` may be null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instantiate(
    store: *mut WasmStore,
    module: *const WasmModule,
    imports: *const WasmExtern,
    import_count: usize,
    instance: *mut u32,
    error: *mut WasmString,
) -> WasmStatus {
    let mut values = Vec::with_capacity(import_count);
    for import in unsafe { ak::slice_from_raw_parts(imports, import_count) } {
        values.push(import.to_extern_value());
    }
    let module = unsafe { (*module).0.clone() };
    match store_of(store).instantiate(module, &values) {
        Ok(address) => {
            unsafe { *instance = address.0 };
            WasmStatus::Ok
        }
        Err(InstantiationError::Link(message)) => {
            write_string(error, &message);
            WasmStatus::LinkError
        }
        Err(InstantiationError::Trap(trap)) => {
            write_string(error, &trap.message());
            WasmStatus::Trap
        }
    }
}

/// The number of exports of an instance.
///
/// # Safety
///
/// `store` must be a live store with an instance at `instance`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_instance_export_count(store: *mut WasmStore, instance: u32) -> usize {
    store_of(store).instance(InstanceAddress(instance)).module.exports.len()
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
    instance: u32,
    index: usize,
    name: *mut WasmStringView,
    value: *mut WasmExtern,
) -> bool {
    let instance = store_of(store).instance(InstanceAddress(instance));
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
    instance: u32,
    name: *const u8,
    length: usize,
    value: *mut WasmExtern,
) -> bool {
    let name = unsafe { ak::slice_from_raw_parts(name, length) };
    let Ok(name) = std::str::from_utf8(name) else {
        return false;
    };
    let Some(extern_value) = store_of(store).instance(InstanceAddress(instance)).export(name) else {
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
    function: u32,
    parameter_count: *mut usize,
    result_count: *mut usize,
) {
    let ty = &store_of(store).function(FunctionAddress(function)).ty;
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
    function: u32,
    parameters: *mut WasmValueType,
    results: *mut WasmValueType,
) {
    let ty = &store_of(store).function(FunctionAddress(function)).ty;
    unsafe { write_function_type(ty, parameters, results) };
}

/// Call a function. On a trap, writes its message to `error`.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`, `arguments` must hold one value for each parameter,
/// `results` must have room for one value for each result, and `error` must be null or writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_invoke(
    store: *mut WasmStore,
    function: u32,
    arguments: *const WasmValue,
    argument_count: usize,
    results: *mut WasmValue,
    result_count: usize,
    error: *mut WasmString,
) -> WasmStatus {
    let arguments: Vec<Value> = unsafe { ak::slice_from_raw_parts(arguments, argument_count) }
        .iter()
        .map(|value| value.to_value())
        .collect();
    match store_of(store).invoke(FunctionAddress(function), &arguments) {
        Ok(values) => {
            for (index, value) in values.into_iter().take(result_count).enumerate() {
                unsafe { *results.add(index) = WasmValue::of(value) };
            }
            WasmStatus::Ok
        }
        Err(trap) => {
            write_string(error, &trap.message());
            WasmStatus::Trap
        }
    }
}

/// The reference to a function, as tables and reference values hold it.
///
/// # Safety
///
/// `store` must be a live store with a function at `function`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_function_reference(store: *mut WasmStore, function: u32) -> u64 {
    store_of(store).function_reference(FunctionAddress(function))
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
    function: *mut u32,
) -> bool {
    let Some(address) = store_of(store).function_from_reference(reference) else {
        return false;
    };
    unsafe { *function = address.0 };
    true
}

/// The bytes of a memory, which move when it grows.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`, and `size` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_data(store: *mut WasmStore, memory: u32, size: *mut usize) -> *mut u8 {
    let memory = store_of(store).memory_mut(MemoryAddress(memory));
    unsafe { *size = memory.size() };
    memory.bytes_mut().as_mut_ptr()
}

/// Grow a memory by `delta` pages. Returns the previous size in pages, or -1 if it cannot grow.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_grow(store: *mut WasmStore, memory: u32, delta: u64) -> i64 {
    store_of(store)
        .memory_mut(MemoryAddress(memory))
        .grow(delta)
        .map_or(-1, |previous| previous as i64)
}

/// The limits a memory was created with.
///
/// # Safety
///
/// `store` must be a live store with a memory at `memory`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_memory_limits(store: *mut WasmStore, memory: u32) -> WasmLimits {
    WasmLimits::of(&store_of(store).memory(MemoryAddress(memory)).ty.limits)
}

/// # Safety
///
/// `store` must be a live store with a table at `table`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_size(store: *mut WasmStore, table: u32) -> u64 {
    store_of(store).table(TableAddress(table)).elements.len() as u64
}

/// The element type and limits a table was created with.
///
/// # Safety
///
/// `store` must be a live store with a table at `table`, and the out parameters must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_table_type(
    store: *mut WasmStore,
    table: u32,
    element: *mut WasmValueType,
    limits: *mut WasmLimits,
) {
    let table = store_of(store).table(TableAddress(table));
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
pub unsafe extern "C" fn wasm_store_table_get(store: *mut WasmStore, table: u32, index: u64, value: *mut u64) -> bool {
    let Some(&element) = store_of(store).table(TableAddress(table)).elements.get(index as usize) else {
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
pub unsafe extern "C" fn wasm_store_table_set(store: *mut WasmStore, table: u32, index: u64, value: u64) -> bool {
    let Some(element) = store_of(store)
        .table_mut(TableAddress(table))
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
pub unsafe extern "C" fn wasm_store_table_grow(store: *mut WasmStore, table: u32, delta: u64, initial: u64) -> i64 {
    store_of(store)
        .table_mut(TableAddress(table))
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
    global: u32,
    mutable: *mut bool,
) -> WasmValueType {
    let ty = store_of(store).global(GlobalAddress(global)).ty;
    unsafe { *mutable = ty.mutable };
    WasmValueType::of(ty.value)
}

/// # Safety
///
/// `store` must be a live store with a global at `global`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_global_get(store: *mut WasmStore, global: u32) -> WasmValue {
    WasmValue::of(store_of(store).global(GlobalAddress(global)).get())
}

/// # Safety
///
/// `store` must be a live store with a global at `global`, and `value` must have the global's type.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wasm_store_global_set(store: *mut WasmStore, global: u32, value: WasmValue) {
    store_of(store).global_mut(GlobalAddress(global)).set(value.to_value());
}
