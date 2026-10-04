/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Realms, their intrinsics and their host-defined data, and the host-defined data of JobCallback Records.
//!
//! Every function here runs on the thread that owns the VM. Realms, objects and job callbacks cross as the addresses
//! of their cells, which stay alive while something keeps them alive, conservative stack scanning included. A
//! host-defined cell is a cell of the VM's heap, normally one of the embedder's C++ GC cells, which the record that
//! holds it keeps alive.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::embedding::abi_types::{CellAbi, JSRealm, cell_from_abi, cell_into_abi, completion_into_abi, vm_from_abi};
use crate::gc::foreign::ForeignCellSlot;
use crate::layout::cell::Gc;
use crate::layout::execution_context::ExecutionContext;
use crate::layout::host_class::{JSCompletion, JSObject, JSVM};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::get_function_realm;
use crate::runtime::job_callback::JobCallback;

/// A JobCallback Record, which C only ever sees behind a pointer.
pub struct JSJobCallback {
    _opaque: [u8; 0],
}

impl CellAbi for JSJobCallback {
    type Cell = JobCallback;
}

/// What InitializeHostDefinedRealm calls for an object created in a host-defined manner: the realm's global object, or
/// the this value of its global environment. It gets the `context` it was passed with, and the realm, whose
/// intrinsics exist but whose global object and global environment do not yet. It must return an object.
pub type JSCreateRealmObjectCallback =
    Option<unsafe extern "C" fn(context: *mut c_void, realm: *mut JSRealm) -> *mut JSObject>;

/// Names an intrinsic of a realm for js_realm_intrinsic().
pub type JSIntrinsic = u32;

pub const JS_INTRINSIC_OBJECT_PROTOTYPE: JSIntrinsic = 0;
pub const JS_INTRINSIC_FUNCTION_PROTOTYPE: JSIntrinsic = 1;
pub const JS_INTRINSIC_ARRAY_PROTOTYPE: JSIntrinsic = 2;
pub const JS_INTRINSIC_STRING_PROTOTYPE: JSIntrinsic = 3;
pub const JS_INTRINSIC_ERROR_PROTOTYPE: JSIntrinsic = 4;
pub const JS_INTRINSIC_ITERATOR_PROTOTYPE: JSIntrinsic = 5;
pub const JS_INTRINSIC_ASYNC_ITERATOR_PROTOTYPE: JSIntrinsic = 6;
pub const JS_INTRINSIC_ERROR_CONSTRUCTOR: JSIntrinsic = 7;
pub const JS_INTRINSIC_PROMISE_CONSTRUCTOR: JSIntrinsic = 8;
pub const JS_INTRINSIC_SHARED_ARRAY_BUFFER_CONSTRUCTOR: JSIntrinsic = 9;
pub const JS_INTRINSIC_DATA_VIEW_CONSTRUCTOR: JSIntrinsic = 10;
pub const JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR: JSIntrinsic = 11;
pub const JS_INTRINSIC_UINT8_CLAMPED_ARRAY_CONSTRUCTOR: JSIntrinsic = 12;
pub const JS_INTRINSIC_UINT16_ARRAY_CONSTRUCTOR: JSIntrinsic = 13;
pub const JS_INTRINSIC_UINT32_ARRAY_CONSTRUCTOR: JSIntrinsic = 14;
pub const JS_INTRINSIC_BIG_UINT64_ARRAY_CONSTRUCTOR: JSIntrinsic = 15;
pub const JS_INTRINSIC_INT8_ARRAY_CONSTRUCTOR: JSIntrinsic = 16;
pub const JS_INTRINSIC_INT16_ARRAY_CONSTRUCTOR: JSIntrinsic = 17;
pub const JS_INTRINSIC_INT32_ARRAY_CONSTRUCTOR: JSIntrinsic = 18;
pub const JS_INTRINSIC_BIG_INT64_ARRAY_CONSTRUCTOR: JSIntrinsic = 19;
pub const JS_INTRINSIC_FLOAT16_ARRAY_CONSTRUCTOR: JSIntrinsic = 20;
pub const JS_INTRINSIC_FLOAT32_ARRAY_CONSTRUCTOR: JSIntrinsic = 21;
pub const JS_INTRINSIC_FLOAT64_ARRAY_CONSTRUCTOR: JSIntrinsic = 22;
pub const JS_INTRINSIC_JSON_STRINGIFY_FUNCTION: JSIntrinsic = 23;
pub const JS_INTRINSIC_CONSOLE_OBJECT: JSIntrinsic = 24;
/// One more than the largest JSIntrinsic.
pub const JS_INTRINSIC_COUNT: JSIntrinsic = 25;

/// A slot holding `cell`, or an empty one for null.
///
/// # Safety
///
/// `cell` must be null or the address of a live cell of the VM's heap.
pub(crate) unsafe fn host_defined_slot_of(cell: *mut c_void) -> ForeignCellSlot {
    let slot = ForeignCellSlot::empty();
    // SAFETY: The caller passes null or a live cell, which the slot keeps alive from now on.
    unsafe { slot.set(NonNull::new(cell)) };
    slot
}

/// InitializeHostDefinedRealm: creates a realm with its intrinsics, global object and global environment, and runs
/// it in a new execution context, which it constructs in `new_context` and pushes onto the execution context stack.
/// The context's realm is the new realm, and the context stays on the stack, as the running execution context, until
/// the caller pops it.
///
/// `new_context` is storage for an execution context without slots, at least JS_LAYOUT_EXECUTION_CONTEXT_SIZE bytes
/// aligned to JS_LAYOUT_EXECUTION_CONTEXT_ALIGN, which the caller owns and keeps alive while the context is on the
/// stack. An embedder that allocates contexts can pass one without slots that is on no stack, which is constructed
/// anew.
///
/// Without a create_global_object callback the global object is an ordinary global object. Without a
/// create_global_this_value callback the this value of the global environment is the global object. The callbacks may
/// call back into the VM; they run while the new context is the running execution context.
///
/// Main thread only.
///
/// # Safety
///
/// `vm` must be the live VM, `new_context` must be storage as described, and each callback must return a live object
/// of the VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_initialize_host_defined_realm(
    vm: *mut JSVM,
    new_context: *mut c_void,
    create_global_object: JSCreateRealmObjectCallback,
    create_global_object_context: *mut c_void,
    create_global_this_value: JSCreateRealmObjectCallback,
    create_global_this_value_context: *mut c_void,
) -> JSCompletion {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    let new_context = new_context.cast::<ExecutionContext>();
    assert!(
        !new_context.is_null() && new_context.is_aligned(),
        "the embedder passes aligned storage for the realm's execution context"
    );
    // SAFETY: The caller passes storage for a context without slots, which nothing else uses.
    let new_context = unsafe {
        ExecutionContext::initialize_at(new_context, 0, 0, 0, 0);
        &*new_context
    };
    let call = |callback: unsafe extern "C" fn(*mut c_void, *mut JSRealm) -> *mut JSObject,
                context: *mut c_void,
                realm: Gc<Realm>| {
        // SAFETY: The embedder's callback creates an object for the realm, which is live.
        let object = unsafe { callback(context, cell_into_abi(realm)) };
        // SAFETY: The callback returns a live object of the VM.
        unsafe { cell_from_abi(object) }
    };
    let create_global_object =
        create_global_object.map(|callback| move |realm| call(callback, create_global_object_context, realm));
    let create_global_this_value =
        create_global_this_value.map(|callback| move |realm| call(callback, create_global_this_value_context, realm));
    let result = Realm::initialize_host_defined_realm_in(
        vm,
        new_context,
        create_global_object
            .as_ref()
            .map(|callback| callback as &dyn Fn(Gc<Realm>) -> Gc<Object>),
        create_global_this_value
            .as_ref()
            .map(|callback| callback as &dyn Fn(Gc<Realm>) -> Gc<Object>),
    );
    completion_into_abi(result.map(|_| ()))
}

/// Sets the realm's [[HostDefined]] to `host_defined`, or clears it for null. The realm keeps the cell alive for as
/// long as it lives itself. Main thread only.
///
/// # Safety
///
/// `realm` must be a live realm, and `host_defined` null or a live cell of the VM's heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_set_host_defined(realm: *mut JSRealm, host_defined: *mut c_void) {
    // SAFETY: The caller passes a live realm.
    let realm = unsafe { cell_from_abi(realm) };
    // SAFETY: The caller passes null or a live cell, which the realm keeps alive from now on.
    unsafe { realm.host_defined.set(NonNull::new(host_defined)) };
}

/// The intrinsic `intrinsic` names of the realm, which this creates if the realm has not needed it yet, or null for a
/// JSIntrinsic this runtime does not know. Main thread only.
///
/// # Safety
///
/// `vm` must be the live VM and `realm` a live realm whose intrinsics exist.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_intrinsic(
    vm: *mut JSVM,
    realm: *mut JSRealm,
    intrinsic: JSIntrinsic,
) -> *mut JSObject {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live realm.
    let realm = unsafe { cell_from_abi(realm) };
    let intrinsics = realm.intrinsics();
    let object: Gc<Object> = match intrinsic {
        JS_INTRINSIC_OBJECT_PROTOTYPE => intrinsics.object_prototype(vm),
        JS_INTRINSIC_FUNCTION_PROTOTYPE => intrinsics.function_prototype(vm),
        JS_INTRINSIC_ARRAY_PROTOTYPE => intrinsics.array_prototype(vm),
        JS_INTRINSIC_STRING_PROTOTYPE => intrinsics.string_prototype(vm),
        JS_INTRINSIC_ERROR_PROTOTYPE => intrinsics.error_prototype(vm),
        JS_INTRINSIC_ITERATOR_PROTOTYPE => intrinsics.iterator_prototype(vm),
        JS_INTRINSIC_ASYNC_ITERATOR_PROTOTYPE => intrinsics.async_iterator_prototype(),
        JS_INTRINSIC_ERROR_CONSTRUCTOR => intrinsics.error_constructor(vm).upcast(),
        JS_INTRINSIC_PROMISE_CONSTRUCTOR => intrinsics.promise_constructor(vm).upcast(),
        JS_INTRINSIC_SHARED_ARRAY_BUFFER_CONSTRUCTOR => intrinsics.shared_array_buffer_constructor(vm).upcast(),
        JS_INTRINSIC_DATA_VIEW_CONSTRUCTOR => intrinsics.data_view_constructor(vm).upcast(),
        JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR => intrinsics.uint8_array_constructor(vm).upcast(),
        JS_INTRINSIC_UINT8_CLAMPED_ARRAY_CONSTRUCTOR => intrinsics.uint8_clamped_array_constructor(vm).upcast(),
        JS_INTRINSIC_UINT16_ARRAY_CONSTRUCTOR => intrinsics.uint16_array_constructor(vm).upcast(),
        JS_INTRINSIC_UINT32_ARRAY_CONSTRUCTOR => intrinsics.uint32_array_constructor(vm).upcast(),
        JS_INTRINSIC_BIG_UINT64_ARRAY_CONSTRUCTOR => intrinsics.big_uint64_array_constructor(vm).upcast(),
        JS_INTRINSIC_INT8_ARRAY_CONSTRUCTOR => intrinsics.int8_array_constructor(vm).upcast(),
        JS_INTRINSIC_INT16_ARRAY_CONSTRUCTOR => intrinsics.int16_array_constructor(vm).upcast(),
        JS_INTRINSIC_INT32_ARRAY_CONSTRUCTOR => intrinsics.int32_array_constructor(vm).upcast(),
        JS_INTRINSIC_BIG_INT64_ARRAY_CONSTRUCTOR => intrinsics.big_int64_array_constructor(vm).upcast(),
        JS_INTRINSIC_FLOAT16_ARRAY_CONSTRUCTOR => intrinsics.float16_array_constructor(vm).upcast(),
        JS_INTRINSIC_FLOAT32_ARRAY_CONSTRUCTOR => intrinsics.float32_array_constructor(vm).upcast(),
        JS_INTRINSIC_FLOAT64_ARRAY_CONSTRUCTOR => intrinsics.float64_array_constructor(vm).upcast(),
        JS_INTRINSIC_JSON_STRINGIFY_FUNCTION => intrinsics.json_stringify_function().upcast(),
        JS_INTRINSIC_CONSOLE_OBJECT => intrinsics.console_object(vm).upcast(),
        _ => return core::ptr::null_mut(),
    };
    cell_into_abi(object)
}

/// GetFunctionRealm: the realm of `function`, a function object, as a JSRealm that is the payload of the normal
/// completion. Throws a TypeError for a revoked proxy. Main thread only.
///
/// # Safety
///
/// `vm` must be the live VM, and `function` a live function object of it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_get_function_realm(vm: *mut JSVM, function: *mut JSObject) -> JSCompletion {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live object.
    let function = unsafe { cell_from_abi(function) };
    assert!(function.is_function(), "GetFunctionRealm is given a function object");
    completion_into_abi(
        get_function_realm(vm, Value::from_object(function).as_function()).map(cell_into_abi::<JSRealm>),
    )
}

/// A JobCallback Record of `callback`, a function object, with `custom_data` as its [[HostDefined]], or none for null,
/// as HostMakeJobCallback creates it. The record keeps the cell alive. Main thread only.
///
/// # Safety
///
/// `vm` must be the live VM, `callback` a live function object of it, and `custom_data` null or a live cell of the
/// VM's heap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_job_callback_create(
    vm: *mut JSVM,
    callback: *mut JSObject,
    custom_data: *mut c_void,
) -> *mut JSJobCallback {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live object.
    let callback = unsafe { cell_from_abi(callback) };
    assert!(callback.is_function(), "a JobCallback Record calls a function object");
    // SAFETY: The caller passes null or a live cell.
    let custom_data = unsafe { host_defined_slot_of(custom_data) };
    let job_callback = JobCallback::create(vm, Value::from_object(callback).as_function(), custom_data);
    cell_into_abi(job_callback)
}

/// The [[Callback]] of a JobCallback Record. Main thread only.
///
/// # Safety
///
/// `job_callback` must be a live JobCallback Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_job_callback_callback(job_callback: *mut JSJobCallback) -> *mut JSObject {
    // SAFETY: The caller passes a live JobCallback Record.
    let job_callback = unsafe { cell_from_abi(job_callback) };
    cell_into_abi(job_callback.callback().upcast::<Object>())
}

/// The [[HostDefined]] cell of a JobCallback Record, or null if it has none. Main thread only.
///
/// # Safety
///
/// `job_callback` must be a live JobCallback Record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_realm_job_callback_custom_data(job_callback: *mut JSJobCallback) -> *mut c_void {
    // SAFETY: The caller passes a live JobCallback Record.
    let job_callback = unsafe { cell_from_abi(job_callback) };
    job_callback
        .custom_data()
        .map_or(core::ptr::null_mut(), NonNull::as_ptr)
}
