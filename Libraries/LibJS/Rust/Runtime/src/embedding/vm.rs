/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constructing the VM in storage the embedder provides, and the VM's state.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::embedding::abi_types::{JSRealm, cell_from_abi, completion_into_abi, optional_cell_from_abi, vm_from_abi};
use crate::embedding::debugger::{JSDebuggerStackFrame, stack_frame_into_abi};
use crate::embedding::hooks::{
    Embedder, EmbedderAgent, JSAgent, JSImportedModulePayload, JSImportedModuleReferrer, JSModuleRequest, JSPromiseJob,
    JSVmHostHooks, imported_module_payload_from_abi, imported_module_referrer_from_abi, install_embedder,
    module_request_from_abi, promise_job_from_abi,
};
use crate::embedding::realm::JSJobCallback;
use crate::gc::capi::GCHeap;
use crate::interpreter::vm::{
    Vm, VmOptions, default_host_enqueue_finalization_registry_cleanup_job, default_host_enqueue_promise_job,
    default_host_grow_shared_array_buffer, default_host_promise_job_queue_is_empty, default_host_resize_array_buffer,
    default_host_system_utc_epoch_nanoseconds,
};
use crate::layout::host_class::{JSCompletion, JSObject, JSVM, JSValue};
use crate::layout::value::Value;
use crate::layout::vm::{VM_ALIGN, VM_SIZE};
use crate::runtime::agent::AgentRecord;
use crate::runtime::array_buffer::ArrayBuffer;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::job_callback::call_job_callback;

// LibJS/Embedding/Layout.h tells an embedder to reserve this much storage for the VM.
const _: () = assert!(size_of::<Vm>() <= VM_SIZE, "the Vm outgrew VM_SIZE in src/layout/vm.rs");
const _: () = assert!(
    align_of::<Vm>() <= VM_ALIGN,
    "the Vm outgrew VM_ALIGN in src/layout/vm.rs"
);

/// How a VM is set up, which is fixed once it is constructed. All false is the setup of a standalone runtime.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct JSVmOptions {
    /// Makes the VM's heap the one GC::Heap::the() returns, so that C++ cells of the embedder are allocated from it.
    pub become_process_default_heap: bool,
    /// Backs SharedArrayBuffers with shared memory, which other processes can map.
    pub shared_memory_shared_array_buffers: bool,
}

impl From<JSVmOptions> for VmOptions {
    fn from(options: JSVmOptions) -> Self {
        Self {
            become_process_default_heap: options.become_process_default_heap,
            shared_memory_shared_array_buffers: options.shared_memory_shared_array_buffers,
        }
    }
}

/// Constructs a VM in the embedder's `storage` of `size` bytes, aligned to `align`, where the VM then has to stay until
/// js_vm_destroy_at(). The embedder keeps owning the storage. JS_LAYOUT_VM_SIZE and JS_LAYOUT_VM_ALIGN are always
/// enough. Returns false, and constructs nothing, if the storage is smaller or less aligned than the VM needs.
/// `options` may be null for the setup of a standalone runtime. The VM's heap belongs to the calling thread, which is
/// the only one that may use the VM from then on.
///
/// # Safety
///
/// `storage` must be valid for writes of `size` bytes and aligned to `align`. `options` must be null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_construct_at(
    storage: *mut c_void,
    size: usize,
    align: usize,
    options: *const JSVmOptions,
) -> bool {
    if storage.is_null() || size < size_of::<Vm>() || align < align_of::<Vm>() || !storage.cast::<Vm>().is_aligned() {
        return false;
    }
    // SAFETY: The caller passes null or valid options.
    let options = unsafe { options.as_ref() }.copied().unwrap_or_default();
    // SAFETY: The storage is large and aligned enough for a Vm, and the embedder keeps the VM there until it destroys it.
    unsafe { Vm::create_at(storage.cast(), options.into()) };
    true
}

/// Destroys a VM that js_vm_construct_at() constructed, finalizing every cell of its heap, and leaves its storage to
/// the embedder. Only the thread that constructed the VM may call this.
///
/// # Safety
///
/// `vm` must be a live VM, which nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_destroy_at(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM that it gives up.
    unsafe { core::ptr::drop_in_place(vm.cast::<Vm>()) };
}

/// The LibGC heap of the VM, a GC::Heap that the VM owns. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_heap(vm: *mut JSVM) -> *mut GCHeap {
    // SAFETY: The caller passes a live VM.
    unsafe { vm_from_abi(vm) }.heap().raw()
}

/// VM::finish_execution_generation(): ends the synchronous run of code during which WeakRef targets that were
/// dereferenced stay alive, as an embedder does at the end of each microtask checkpoint. Only the VM's thread may call
/// this.
///
/// # Safety
///
/// `vm` must be a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_finish_execution_generation(vm: *mut JSVM) {
    // SAFETY: The caller passes a live VM.
    let generation = &unsafe { vm_from_abi(vm) }.head.execution_generation;
    generation.set(generation.get() + 1);
}

/// VM::did_reach_stack_space_limit(): whether so little of the thread's native stack is left that running more
/// JavaScript could overflow it, which code that recurses on behalf of JavaScript, such as structured serialization,
/// checks before it goes deeper. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_did_reach_stack_space_limit(vm: *mut JSVM) -> bool {
    // SAFETY: The caller passes a live VM.
    unsafe { vm_from_abi(vm) }.did_reach_stack_space_limit()
}

/// Receives each StackTraceElement of a stack trace, which lives for the call.
#[repr(C)]
pub struct JSStackTraceSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, element: *const JSDebuggerStackFrame)>,
}

/// VM::stack_trace(): appends a StackTraceElement for every execution context on the stack, from the running one
/// outwards, with where in its source code each one is, which is none for a context that runs no bytecode, such as
/// that of a native function. The trace is taken before the first element is appended, and the sink may run
/// JavaScript as long as it leaves the contexts of the trace on the stack. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `sink` a valid sink with an append function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_stack_trace(vm: *mut JSVM, sink: *const JSStackTraceSink) {
    // SAFETY: The caller passes a live VM and a valid sink.
    let (vm, sink) = unsafe { (vm_from_abi(vm), &*sink) };
    let append = sink.append.expect("a stack trace sink has an append function");
    for element in vm.stack_trace() {
        let element = stack_frame_into_abi(&element);
        // SAFETY: The embedder's sink takes elements with the context it came with, and only reads them during the
        //         call, while the trace's contexts and their executables are alive.
        unsafe { append(sink.context, &raw const element) };
    }
}

/// Forgets the system time zone that Date and Temporal cached, so that they pick up a change of the host's time zone.
/// It is the cache of the process rather than of a VM, and any thread may call this.
#[unsafe(no_mangle)]
pub extern "C" fn js_vm_clear_system_time_zone_cache() {
    crate::runtime::date::clear_system_time_zone_cache();
}

/// Has the VM call the embedder's `hooks`, each with `data`, in place of its own host-defined behavior from now on,
/// instead of the hooks it had before. A null hook, like every hook of a null table, gets the runtime's own behavior.
/// The embedder can call this again at any time, such as whenever it changes which of its hooks it has, and keeps each
/// table and its data alive and unchanged until it installs another table and no call of a hook from it is in
/// progress. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `hooks` null or a valid table of hooks that take `data`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_set_embedder(vm: *mut JSVM, hooks: *const JSVmHostHooks, data: *mut c_void) {
    // SAFETY: The caller passes a live VM, and a table that stays alive while the VM uses it.
    let (vm, hooks) = unsafe { (vm_from_abi(vm), hooks.as_ref()) };
    install_embedder(vm, hooks.map(|hooks| Embedder { hooks, data }));
}

/// Makes `agent` the surrounding agent of the VM, as VM::set_agent() of the C++ runtime does: its [[CanBlock]] says
/// whether Atomics.wait() may block, and await in native code spins its event loop until the awaited promise settles.
/// Without an agent, which a null `agent` sets, Atomics.wait() may block and await runs the VM's own queue of promise
/// jobs, which is empty if an enqueue_promise_job hook takes the jobs. The VM copies the agent, whose data the embedder
/// keeps alive until it sets another agent or destroys the VM. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `agent` null or a valid agent with a spin_event_loop_until function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_set_agent(vm: *mut JSVM, agent: *const JSAgent) {
    // SAFETY: The caller passes a live VM and a valid agent or null.
    let (vm, agent) = unsafe { (vm_from_abi(vm), agent.as_ref()) };
    vm.set_agent(agent.map_or_else(AgentRecord::default, |agent| AgentRecord {
        can_block: agent.can_block,
        embedder_agent: Some(EmbedderAgent::of(agent)),
    }));
}

/// The runtime's own HostEnqueuePromiseJob ( job, realm ), which an enqueue_promise_job hook can fall back to: queues
/// the job in the VM's own queue. `realm` may be null. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM, `job` a promise job of it, and `realm` null or one of its realms.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_enqueue_promise_job(
    vm: *mut JSVM,
    job: *mut JSPromiseJob,
    realm: *mut JSRealm,
) {
    // SAFETY: The caller passes a live VM, promise job and realm or null.
    let (vm, job, realm) = unsafe {
        (
            vm_from_abi(vm),
            promise_job_from_abi(job),
            optional_cell_from_abi(realm),
        )
    };
    default_host_enqueue_promise_job(vm, job, realm);
}

/// The runtime's own counterpart of a promise_job_queue_is_empty hook: whether the VM's own queue of promise jobs is
/// empty. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_promise_job_queue_is_empty(vm: *mut JSVM) -> bool {
    // SAFETY: The caller passes a live VM.
    default_host_promise_job_queue_is_empty(unsafe { vm_from_abi(vm) })
}

/// The runtime's own HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ), which an
/// enqueue_finalization_registry_cleanup_job hook can fall back to: queues the cleanup in the VM's own queue. It runs
/// no JavaScript. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `finalization_registry` one of its FinalizationRegistry objects.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_enqueue_finalization_registry_cleanup_job(
    vm: *mut JSVM,
    finalization_registry: *mut JSObject,
) {
    // SAFETY: The caller passes a live VM and object.
    let (vm, finalization_registry) = unsafe { (vm_from_abi(vm), cell_from_abi(finalization_registry)) };
    let finalization_registry = finalization_registry
        .downcast::<FinalizationRegistry>()
        .expect("the embedder passes a FinalizationRegistry");
    default_host_enqueue_finalization_registry_cleanup_job(vm, finalization_registry);
}

/// The runtime's own HostLoadImportedModule ( referrer, moduleRequest, hostDefined, payload ), which a
/// load_imported_module hook can fall back to with the arguments it got: loads the module from a file, resolved
/// against the referrer's, and finishes loading it before it returns. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM, and the other arguments those that the VM passed to a load_imported_module hook whose call
/// is in progress.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_load_imported_module(
    vm: *mut JSVM,
    referrer: JSImportedModuleReferrer,
    module_request: *const JSModuleRequest,
    host_defined: *mut c_void,
    payload: JSImportedModulePayload,
) {
    // SAFETY: The caller passes a live VM, and the referrer, module request and payload of a hook call in progress.
    let (vm, referrer, module_request, payload) = unsafe {
        (
            vm_from_abi(vm),
            imported_module_referrer_from_abi(referrer),
            module_request_from_abi(module_request),
            imported_module_payload_from_abi(payload),
        )
    };
    Vm::load_imported_module(vm, referrer, module_request, NonNull::new(host_defined), payload);
}

/// The runtime's own HostSystemUTCEpochNanoseconds ( global ), which a system_utc_epoch_nanoseconds hook can fall
/// back to: the time of the system clock. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `global` one of its global objects.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_system_utc_epoch_nanoseconds(vm: *mut JSVM, global: *mut JSObject) -> i64 {
    // SAFETY: The caller passes a live VM and object.
    let (vm, global) = unsafe { (vm_from_abi(vm), cell_from_abi(global)) };
    let nanoseconds = default_host_system_utc_epoch_nanoseconds(vm, &global);
    i64::try_from(&nanoseconds).expect("the time of the system clock saturates to the range of i64")
}

/// The runtime's own HostResizeArrayBuffer ( buffer, newByteLength ), which a resize_array_buffer hook falls back to
/// for the buffers it does not handle itself: sets the byte length of the storage the buffer owns, or of the buffer
/// whose storage it aliases, to `new_byte_length`, zero-filling new bytes, and completes normally with
/// JS_HANDLED_BY_HOST_HANDLED, or throws a RangeError, with the buffer unchanged, if there is not enough memory. Only
/// the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `buffer` one of its ArrayBuffers that is not fixed-length, as
/// ArrayBuffer.prototype.resize checks before it calls the hook, over storage that it owns or that an ArrayBuffer owns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_resize_array_buffer(
    vm: *mut JSVM,
    buffer: *mut JSObject,
    new_byte_length: usize,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and object.
    let (vm, buffer) = unsafe { (vm_from_abi(vm), cell_from_abi(buffer)) };
    let buffer = buffer
        .downcast::<ArrayBuffer>()
        .expect("the embedder passes an ArrayBuffer");
    // NB: Typed arrays cache where the bytes of a fixed-length buffer are and how many there are.
    assert!(
        !buffer.is_fixed_length(),
        "the embedder resizes a buffer that is not fixed-length"
    );
    completion_into_abi(default_host_resize_array_buffer(vm, &buffer, new_byte_length))
}

/// The runtime's own HostGrowSharedArrayBuffer ( buffer, newByteLength ), which a grow_shared_array_buffer hook can
/// fall back to: completes normally with JS_HANDLED_BY_HOST_UNHANDLED, so that the engine grows the buffer itself.
/// Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM and `buffer` one of its SharedArrayBuffers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_grow_shared_array_buffer(
    vm: *mut JSVM,
    buffer: *mut JSObject,
    new_byte_length: usize,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and object.
    let (vm, buffer) = unsafe { (vm_from_abi(vm), cell_from_abi(buffer)) };
    let buffer = buffer
        .downcast::<ArrayBuffer>()
        .expect("the embedder passes a SharedArrayBuffer");
    completion_into_abi(default_host_grow_shared_array_buffer(vm, &buffer, new_byte_length))
}

/// The runtime's own HostCallJobCallback ( jobCallback, V, argumentsList ): the completion of calling the callback with
/// `argument_count` arguments at `arguments`. Only the VM's thread may call this.
///
/// # Safety
///
/// `vm` must be a live VM, `job_callback` one of its JobCallback Records, and `arguments` valid for `argument_count`
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn js_vm_default_host_call_job_callback(
    vm: *mut JSVM,
    job_callback: *mut JSJobCallback,
    this_value: JSValue,
    arguments: *const JSValue,
    argument_count: usize,
) -> JSCompletion {
    // SAFETY: The caller passes a live VM and JobCallback Record.
    let (vm, job_callback) = unsafe { (vm_from_abi(vm), cell_from_abi(job_callback)) };
    let arguments: &[Value] = if argument_count == 0 {
        &[]
    } else {
        // SAFETY: The caller passes `argument_count` values, and a Value is a JSValue.
        unsafe { core::slice::from_raw_parts(arguments.cast(), argument_count) }
    };
    completion_into_abi(call_job_callback(vm, job_callback, Value(this_value), arguments))
}
