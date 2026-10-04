/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host hooks, which are the embedder's table of callbacks and its data pointer, and the embedder's agent.

use core::ffi::c_void;
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use ak::Utf16String;

use crate::embedding::abi_types::{
    CompletionPayload, JSRealm, JSUtf16View, cell_from_abi, cell_into_abi, completion_from_abi, object_into_abi,
    optional_cell_into_abi, vm_into_abi,
};
use crate::embedding::realm::JSJobCallback;
use crate::gc::heap_function::HeapFunction;
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::{
    CompilationType, HandledByHost, OnUnimplementedPropertyAccess, Vm,
    default_host_enqueue_finalization_registry_cleanup_job, default_host_enqueue_promise_job,
    default_host_ensure_can_add_private_element, default_host_ensure_can_compile_strings,
    default_host_finalize_import_meta, default_host_get_code_for_eval, default_host_get_import_meta_properties,
    default_host_get_supported_import_attributes, default_host_grow_shared_array_buffer,
    default_host_promise_job_queue_is_empty, default_host_promise_rejection_tracker, default_host_resize_array_buffer,
    default_host_system_utc_epoch_nanoseconds, default_host_unrecognized_date_string,
};
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::host_class::{JSCompletion, JSModule, JSObject, JSPropertyKey, JSStringSink, JSVM, JSValue};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::runtime::array_buffer::ArrayBuffer;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::cyclic_module::CyclicModule;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::job_callback::{self, JobCallback};
use crate::runtime::module::GraphLoadingState;
use crate::runtime::module_loading::{ImportedModulePayload, ImportedModuleReferrer};
use crate::runtime::module_request::ModuleRequest;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise::{Promise, RejectionOperation};
use crate::runtime::promise_capability::PromiseCapability;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use crate::utf16::Utf16View;

/// The [[Job]] of a PromiseJob Record, which the embedder runs once with js_promise_job_run(). It is a LibGC cell,
/// which the embedder keeps alive until then by visiting it from one of its cells or by rooting it.
// NB: Without repr(C), cbindgen declares the type without a definition, which is all C needs.
pub struct JSPromiseJob {
    _opaque: [u8; 0],
}

/// A ModuleRequest Record, which a hook only borrows for the duration of its call.
pub struct JSModuleRequest {
    _opaque: [u8; 0],
}

/// The compilationType of HostEnsureCanCompileStrings, in the order of the C++ JS::CompilationType.
pub const JS_COMPILATION_TYPE_DIRECT_EVAL: u8 = 0;
pub const JS_COMPILATION_TYPE_INDIRECT_EVAL: u8 = 1;
pub const JS_COMPILATION_TYPE_FUNCTION: u8 = 2;
pub const JS_COMPILATION_TYPE_TIMER: u8 = 3;

/// The operation of HostPromiseRejectionTracker, in the order of the C++ JS::Promise::RejectionOperation.
pub const JS_PROMISE_REJECTION_OPERATION_REJECT: u8 = 0;
pub const JS_PROMISE_REJECTION_OPERATION_HANDLE: u8 = 1;

/// The normal payload of HostResizeArrayBuffer and HostGrowSharedArrayBuffer, an enumerator in the order of the C++
/// JS::HandledByHost.
pub const JS_HANDLED_BY_HOST_HANDLED: u8 = 0;
pub const JS_HANDLED_BY_HOST_UNHANDLED: u8 = 1;

/// What the record of a JSImportedModuleReferrer is, in the order of the C++ JS::ImportedModuleReferrer.
pub const JS_IMPORTED_MODULE_REFERRER_SCRIPT: u8 = 0;
pub const JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE: u8 = 1;
pub const JS_IMPORTED_MODULE_REFERRER_REALM: u8 = 2;

/// What the record of a JSImportedModulePayload is, in the order of the C++ JS::ImportedModulePayload.
pub const JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE: u8 = 0;
pub const JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY: u8 = 1;

/// The referrer of HostLoadImportedModule: a Script Record, a Cyclic Module Record (a JSModule) or a Realm Record (a
/// JSRealm), as `kind` says.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSImportedModuleReferrer {
    pub kind: u8,
    pub record: *mut c_void,
}

/// The payload of HostLoadImportedModule, a GraphLoadingState Record or a PromiseCapability Record as `kind` says,
/// which the embedder passes on to FinishLoadingImportedModule untouched.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSImportedModulePayload {
    pub kind: u8,
    pub record: *mut c_void,
}

/// The arguments of HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString,
/// compilationType, parameterArgs, bodyArg ), with compilationType one of JS_COMPILATION_TYPE_*.
#[repr(C)]
pub struct JSEnsureCanCompileStringsArguments {
    pub callee_realm: *mut JSRealm,
    pub parameter_strings: *const JSUtf16View,
    pub parameter_string_count: usize,
    pub body_string: JSUtf16View,
    pub code_string: JSUtf16View,
    pub compilation_type: u8,
    pub parameter_args: *const JSValue,
    pub parameter_arg_count: usize,
    pub body_arg: JSValue,
}

/// Where HostGetImportMetaProperties appends each property that import.meta starts with. The key is borrowed for the
/// call, and the value has to be alive until it is appended.
#[repr(C)]
pub struct JSImportMetaPropertySink {
    pub context: *mut c_void,
    pub append: unsafe extern "C" fn(context: *mut c_void, key: JSPropertyKey, value: JSValue),
}

/// Whether the goal of a spin of the event loop is met, given the context that came with it.
pub type JSGoalCondition = unsafe extern "C" fn(goal_context: *mut c_void) -> bool;

/// The surrounding agent of the VM, which the embedder provides: the C++ runtime's JS::Agent.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JSAgent {
    /// [[CanBlock]]: whether Atomics.wait() may block.
    pub can_block: bool,
    /// Agent::spin_event_loop_until(): runs the embedder's event loop, promise jobs included, until
    /// goal_condition(goal_context) is true, which await in native code waits for its promise with. It receives `data`
    /// and the VM, runs on the VM's thread, and may run any JavaScript, including another spin.
    pub spin_event_loop_until: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            goal_condition: JSGoalCondition,
            goal_context: *mut c_void,
        ),
    >,
    pub data: *mut c_void,
}

/// The embedder's table of host hooks, which the VM calls in place of its own host-defined behavior. A null hook keeps
/// the runtime's own behavior, which js_vm_default_host_*() exports wherever it does more than return a fixed answer.
/// Every hook receives the data pointer the embedder installed the table with and the VM, runs on the VM's thread, and
/// may run JavaScript unless it says otherwise. The engine objects and values it receives stay alive for the duration
/// of the call.
#[repr(C)]
pub struct JSVmHostHooks {
    /// HostEnsureCanAddPrivateElement ( O ): a throw completion keeps the private element from being added to the object.
    /// The runtime's own completes normally.
    pub ensure_can_add_private_element:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, object: *mut JSObject) -> JSCompletion>,
    /// HostEnsureCanCompileStrings: a throw completion keeps the strings from being compiled. The runtime's own
    /// completes normally.
    pub ensure_can_compile_strings: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            arguments: *const JSEnsureCanCompileStringsArguments,
        ) -> JSCompletion,
    >,
    /// HostGetCodeForEval ( argument ): returns true and writes a String to `code` for an object that eval() is to run
    /// the code of, and returns false for NO-CODE, as the runtime's own always does.
    pub get_code_for_eval: Option<
        unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, argument: *mut JSObject, code: *mut JSValue) -> bool,
    >,
    /// HostPromiseRejectionTracker ( promise, operation ), with operation one of JS_PROMISE_REJECTION_OPERATION_*.
    /// Without it, the runtime tracks no rejections for the embedder.
    pub promise_rejection_tracker:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, promise: *mut JSObject, operation: u8)>,
    /// HostCallJobCallback ( jobCallback, V, argumentsList ): the completion of calling the callback.
    /// js_vm_default_host_call_job_callback() is the runtime's own.
    pub call_job_callback: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            job_callback: *mut JSJobCallback,
            this_value: JSValue,
            arguments: *const JSValue,
            argument_count: usize,
        ) -> JSCompletion,
    >,
    /// HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ). It runs at the end of a garbage
    /// collection, which any allocation can start, so it queues the cleanup and must not run JavaScript itself.
    /// js_vm_default_host_enqueue_finalization_registry_cleanup_job() is the runtime's own.
    pub enqueue_finalization_registry_cleanup_job:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, finalization_registry: *mut JSObject)>,
    /// HostEnqueuePromiseJob ( job, realm ), where the realm may be null. The embedder runs the job later with
    /// js_promise_job_run(), and keeps it alive until then. js_vm_default_host_enqueue_promise_job() is the runtime's
    /// own.
    pub enqueue_promise_job:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, job: *mut JSPromiseJob, realm: *mut JSRealm)>,
    /// Whether the queue that enqueue_promise_job fills is empty, which an async function asks before it resumes
    /// right after an await. js_vm_default_host_promise_job_queue_is_empty() is the runtime's own.
    pub promise_job_queue_is_empty: Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM) -> bool>,
    /// HostMakeJobCallback ( callable ): a new JobCallback Record of the callable. The runtime's own creates the record
    /// that js_realm_job_callback_create(vm, callable, NULL) does.
    pub make_job_callback:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, callable: *mut JSObject) -> *mut JSJobCallback>,
    /// HostGetImportMetaProperties ( moduleRecord ): appends the properties that import.meta starts with, of which the
    /// runtime's own has none.
    pub get_import_meta_properties: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            module: *mut JSModule,
            properties: *mut JSImportMetaPropertySink,
        ),
    >,
    /// HostFinalizeImportMeta ( importMeta, moduleRecord ). The runtime's own does nothing.
    pub finalize_import_meta: Option<
        unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, import_meta: *mut JSObject, module: *mut JSModule),
    >,
    /// HostGetSupportedImportAttributes ( ): appends the key of each supported import attribute. The runtime's own
    /// supports "type".
    pub get_supported_import_attributes:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, attribute_keys: *mut JSStringSink)>,
    /// HostLoadImportedModule ( referrer, moduleRequest, hostDefined, payload ), which finishes loading the module
    /// now or later with FinishLoadingImportedModule. hostDefined is the cell that LoadRequestedModules was given, or
    /// null for EMPTY. js_vm_default_host_load_imported_module() is the runtime's own, which loads files.
    pub load_imported_module: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            referrer: JSImportedModuleReferrer,
            module_request: *const JSModuleRequest,
            host_defined: *mut c_void,
            payload: JSImportedModulePayload,
        ),
    >,
    /// The Date constructor or Date.parse() found no date in a string, which is borrowed for the call. The runtime's
    /// own does nothing.
    pub unrecognized_date_string:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, date_string: JSUtf16View)>,
    /// HostResizeArrayBuffer ( buffer, newByteLength ): a normal completion carries one of JS_HANDLED_BY_HOST_*.
    /// js_vm_default_host_resize_array_buffer() is the runtime's own.
    pub resize_array_buffer: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            buffer: *mut JSObject,
            new_byte_length: usize,
        ) -> JSCompletion,
    >,
    /// HostGrowSharedArrayBuffer ( buffer, newByteLength ): a normal completion carries one of
    /// JS_HANDLED_BY_HOST_*. js_vm_default_host_grow_shared_array_buffer() is the runtime's own.
    pub grow_shared_array_buffer: Option<
        unsafe extern "C" fn(
            data: *mut c_void,
            vm: *mut JSVM,
            buffer: *mut JSObject,
            new_byte_length: usize,
        ) -> JSCompletion,
    >,
    /// HostSystemUTCEpochNanoseconds ( global ): the current time in nanoseconds since the epoch.
    /// js_vm_default_host_system_utc_epoch_nanoseconds() is the runtime's own.
    pub system_utc_epoch_nanoseconds:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, global: *mut JSObject) -> i64>,
    /// VM::on_unimplemented_property_access: code looked for a property of the object that the embedder declared
    /// unimplemented. The key is borrowed for the call. Without it, nothing happens.
    pub on_unimplemented_property_access:
        Option<unsafe extern "C" fn(data: *mut c_void, vm: *mut JSVM, object: *mut JSObject, key: JSPropertyKey)>,
}

/// What an embedder installs on the VM: its hooks, and the data pointer they receive.
#[derive(Clone, Copy)]
pub struct Embedder {
    pub hooks: &'static JSVmHostHooks,
    pub data: *mut c_void,
}

/// Has the VM call the hooks of `embedder` from now on, in place of those of the embedder it had before. Every hook
/// that the new table does not have, like every hook once there is no embedder, gets the runtime's own behavior back.
pub(crate) fn install_embedder(vm: &Vm, embedder: Option<Embedder>) {
    vm.set_embedder(embedder);
    let hooks = embedder.map(|embedder| embedder.hooks);
    macro_rules! install {
        ($($hook:ident, $set_hook_slot:ident, $runtime_default:path;)*) => {$(
            vm.$set_hook_slot(if hooks.is_some_and(|hooks| hooks.$hook.is_some()) {
                $hook
            } else {
                $runtime_default
            });
        )*};
    }
    install! {
        ensure_can_add_private_element, set_host_ensure_can_add_private_element,
            default_host_ensure_can_add_private_element;
        ensure_can_compile_strings, set_host_ensure_can_compile_strings, default_host_ensure_can_compile_strings;
        get_code_for_eval, set_host_get_code_for_eval, default_host_get_code_for_eval;
        promise_rejection_tracker, set_host_promise_rejection_tracker, default_host_promise_rejection_tracker;
        call_job_callback, set_host_call_job_callback, job_callback::call_job_callback;
        enqueue_finalization_registry_cleanup_job, set_host_enqueue_finalization_registry_cleanup_job,
            default_host_enqueue_finalization_registry_cleanup_job;
        enqueue_promise_job, set_host_enqueue_promise_job, default_host_enqueue_promise_job;
        promise_job_queue_is_empty, set_host_promise_job_queue_is_empty, default_host_promise_job_queue_is_empty;
        make_job_callback, set_host_make_job_callback, job_callback::make_job_callback;
        get_import_meta_properties, set_host_get_import_meta_properties, default_host_get_import_meta_properties;
        finalize_import_meta, set_host_finalize_import_meta, default_host_finalize_import_meta;
        get_supported_import_attributes, set_host_get_supported_import_attributes,
            default_host_get_supported_import_attributes;
        load_imported_module, set_host_load_imported_module, Vm::load_imported_module;
        unrecognized_date_string, set_host_unrecognized_date_string, default_host_unrecognized_date_string;
        resize_array_buffer, set_host_resize_array_buffer, default_host_resize_array_buffer;
        grow_shared_array_buffer, set_host_grow_shared_array_buffer, default_host_grow_shared_array_buffer;
        system_utc_epoch_nanoseconds, set_host_system_utc_epoch_nanoseconds,
            default_host_system_utc_epoch_nanoseconds;
    }
    let has_on_unimplemented_property_access =
        hooks.is_some_and(|hooks| hooks.on_unimplemented_property_access.is_some());
    vm.set_on_unimplemented_property_access(
        has_on_unimplemented_property_access
            .then_some(on_unimplemented_property_access as OnUnimplementedPropertyAccess),
    );
}

/// The event loop of the agent that an embedder provides, which the C++ runtime reaches through VM::agent().
#[derive(Clone, Copy)]
pub struct EmbedderAgent {
    spin_event_loop_until: unsafe extern "C" fn(*mut c_void, *mut JSVM, JSGoalCondition, *mut c_void),
    data: *mut c_void,
}

impl EmbedderAgent {
    pub fn of(agent: &JSAgent) -> Self {
        Self {
            spin_event_loop_until: agent
                .spin_event_loop_until
                .expect("the agent of an embedder spins its event loop"),
            data: agent.data,
        }
    }

    /// Agent::spin_event_loop_until(goal_condition), which may run any JavaScript, including another spin.
    pub fn spin_event_loop_until(&self, vm: &Vm, goal_condition: &dyn Fn() -> bool) {
        unsafe extern "C" fn goal_condition_is_met(goal_condition: *mut c_void) -> bool {
            // SAFETY: The goal context is the goal condition below, which outlives the spin.
            let goal_condition = unsafe { &*goal_condition.cast::<&dyn Fn() -> bool>() };
            goal_condition()
        }
        let goal_context = core::ptr::from_ref(&goal_condition).cast_mut().cast();
        // SAFETY: The embedder's agent takes its data and the VM, and only calls the goal condition during the spin.
        unsafe { (self.spin_event_loop_until)(self.data, vm_into_abi(vm), goal_condition_is_met, goal_context) };
    }
}

/// Calls a hook of the embedder with its data, the VM and `arguments`. The VM holds a hook's thunk only while the
/// embedder's table has the hook, and the caller holds no borrow of VM state, as the hook may run JavaScript.
macro_rules! call_embedder_hook {
    ($vm:expr, $hook:ident $(, $argument:expr)* $(,)?) => {{
        let vm: &Vm = $vm;
        let embedder = vm.embedder().expect("the VM has the embedder whose hook it calls");
        let hook = embedder.hooks.$hook.expect("the VM only calls the hooks the embedder has");
        // SAFETY: The embedder's hook takes its data, the VM, and arguments that are alive for the call.
        unsafe { hook(embedder.data, vm_into_abi(vm) $(, $argument)*) }
    }};
}

/// A key the embedder borrows for the duration of a call.
pub(crate) fn lend_property_key_to_abi(key: &PropertyKey) -> JSPropertyKey {
    // SAFETY: A property key is one word, the bits C sees, and copying them out does not affect its ownership.
    JSPropertyKey {
        bits: unsafe { core::mem::transmute_copy::<PropertyKey, usize>(key) },
    }
}

/// A key of the runtime with the bits of a key that the embedder lends.
///
/// # Safety
///
/// `key` must have the bits of a live property key.
pub(crate) unsafe fn clone_lent_property_key_from_abi(key: JSPropertyKey) -> PropertyKey {
    assert!(key.bits != 0, "the embedder passes a property key");
    // SAFETY: The bits are those of a live key, which the borrowed copy never releases.
    let borrowed = ManuallyDrop::new(unsafe { core::mem::transmute::<usize, PropertyKey>(key.bits) });
    PropertyKey::clone(&borrowed)
}

/// # Safety
///
/// `job` must be a live promise job of the runtime.
pub(crate) unsafe fn promise_job_from_abi(job: *mut JSPromiseJob) -> Gc<HeapFunction> {
    let job = NonNull::new(job.cast()).expect("the embedder passes a promise job");
    // SAFETY: The caller passes a live promise job.
    unsafe { Gc::from_non_null(job) }
}

pub(crate) fn imported_module_referrer_into_abi(referrer: ImportedModuleReferrer) -> JSImportedModuleReferrer {
    let (kind, record) = match referrer {
        ImportedModuleReferrer::Script(script) => (JS_IMPORTED_MODULE_REFERRER_SCRIPT, script.as_ptr().cast()),
        ImportedModuleReferrer::CyclicModule(module) => {
            (JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE, module.as_ptr().cast())
        }
        ImportedModuleReferrer::Realm(realm) => (JS_IMPORTED_MODULE_REFERRER_REALM, realm.as_ptr().cast()),
    };
    JSImportedModuleReferrer { kind, record }
}

/// # Safety
///
/// `referrer` must be one that the runtime passed to load_imported_module, whose record is still alive.
pub unsafe fn imported_module_referrer_from_abi(referrer: JSImportedModuleReferrer) -> ImportedModuleReferrer {
    let record = NonNull::new(referrer.record).expect("a referrer has its record");
    // SAFETY: The caller passes a referrer of the runtime, whose kind says what its live record is.
    unsafe {
        match referrer.kind {
            JS_IMPORTED_MODULE_REFERRER_SCRIPT => {
                ImportedModuleReferrer::Script(Gc::from_non_null(record.cast::<Script>()))
            }
            JS_IMPORTED_MODULE_REFERRER_CYCLIC_MODULE => {
                ImportedModuleReferrer::CyclicModule(Gc::from_non_null(record.cast::<CyclicModule>()))
            }
            JS_IMPORTED_MODULE_REFERRER_REALM => {
                ImportedModuleReferrer::Realm(Gc::from_non_null(record.cast::<Realm>()))
            }
            kind => panic!("the embedder passes a referrer of kind {kind}"),
        }
    }
}

pub(crate) fn imported_module_payload_into_abi(payload: ImportedModulePayload) -> JSImportedModulePayload {
    let (kind, record) = match payload {
        ImportedModulePayload::GraphLoadingState(state) => {
            (JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE, state.as_ptr().cast())
        }
        ImportedModulePayload::PromiseCapability(capability) => (
            JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY,
            capability.as_ptr().cast(),
        ),
    };
    JSImportedModulePayload { kind, record }
}

/// # Safety
///
/// `payload` must be one that the runtime passed to load_imported_module, whose record is still alive.
pub unsafe fn imported_module_payload_from_abi(payload: JSImportedModulePayload) -> ImportedModulePayload {
    let record = NonNull::new(payload.record).expect("a payload has its record");
    // SAFETY: The caller passes a payload of the runtime, whose kind says what its live record is.
    unsafe {
        match payload.kind {
            JS_IMPORTED_MODULE_PAYLOAD_GRAPH_LOADING_STATE => {
                ImportedModulePayload::GraphLoadingState(Gc::from_non_null(record.cast::<GraphLoadingState>()))
            }
            JS_IMPORTED_MODULE_PAYLOAD_PROMISE_CAPABILITY => {
                ImportedModulePayload::PromiseCapability(Gc::from_non_null(record.cast::<PromiseCapability>()))
            }
            kind => panic!("the embedder passes a payload of kind {kind}"),
        }
    }
}

pub(crate) fn module_request_into_abi(module_request: &ModuleRequest) -> *const JSModuleRequest {
    core::ptr::from_ref(module_request).cast()
}

/// # Safety
///
/// `module_request` must be one that the runtime lent to load_imported_module, for the duration of that call.
pub unsafe fn module_request_from_abi<'a>(module_request: *const JSModuleRequest) -> &'a ModuleRequest {
    // SAFETY: The caller passes a module request that the runtime lent it.
    unsafe { &*module_request.cast::<ModuleRequest>() }
}

fn handled_by_host_from_abi(completion: JSCompletion) -> ThrowCompletionOr<HandledByHost> {
    match completion_from_abi(completion)?.0 {
        payload if payload == u64::from(JS_HANDLED_BY_HOST_HANDLED) => Ok(HandledByHost::Handled),
        payload if payload == u64::from(JS_HANDLED_BY_HOST_UNHANDLED) => Ok(HandledByHost::Unhandled),
        payload => panic!("the embedder answered {payload} for whether it handled the buffer"),
    }
}

/// Whether the host handled a buffer, as one of JS_HANDLED_BY_HOST_*.
impl CompletionPayload for HandledByHost {
    fn into_payload(self) -> u64 {
        u64::from(match self {
            HandledByHost::Handled => JS_HANDLED_BY_HOST_HANDLED,
            HandledByHost::Unhandled => JS_HANDLED_BY_HOST_UNHANDLED,
        })
    }
}

/// The address of an object that the VM lends a hook.
fn lent_object_into_abi(object: &Object) -> *mut JSObject {
    // SAFETY: The VM only lends a hook objects that are cells of its heap.
    object_into_abi(unsafe { Gc::from_ref(object) })
}

fn ensure_can_add_private_element(vm: &Vm, object: &Object) -> ThrowCompletionOr<()> {
    completion_from_abi(call_embedder_hook!(
        vm,
        ensure_can_add_private_element,
        lent_object_into_abi(object)
    ))?;
    Ok(())
}

#[allow(clippy::too_many_arguments, reason = "the hook takes the spec's arguments")]
fn ensure_can_compile_strings(
    vm: &Vm,
    callee_realm: Gc<Realm>,
    parameter_strings: &[Utf16String],
    body_string: Utf16View<'_>,
    code_string: Utf16View<'_>,
    compilation_type: CompilationType,
    parameter_args: &[Value],
    body_arg: Value,
) -> ThrowCompletionOr<()> {
    let parameter_strings: Vec<JSUtf16View> = parameter_strings
        .iter()
        .map(|string| JSUtf16View::of(Utf16View::of_string(string)))
        .collect();
    let arguments = JSEnsureCanCompileStringsArguments {
        callee_realm: cell_into_abi(callee_realm),
        parameter_strings: parameter_strings.as_ptr(),
        parameter_string_count: parameter_strings.len(),
        body_string: JSUtf16View::of(body_string),
        code_string: JSUtf16View::of(code_string),
        compilation_type: match compilation_type {
            CompilationType::DirectEval => JS_COMPILATION_TYPE_DIRECT_EVAL,
            CompilationType::IndirectEval => JS_COMPILATION_TYPE_INDIRECT_EVAL,
            CompilationType::Function => JS_COMPILATION_TYPE_FUNCTION,
            CompilationType::Timer => JS_COMPILATION_TYPE_TIMER,
        },
        parameter_args: parameter_args.as_ptr().cast(),
        parameter_arg_count: parameter_args.len(),
        body_arg: body_arg.0,
    };
    completion_from_abi(call_embedder_hook!(
        vm,
        ensure_can_compile_strings,
        &raw const arguments
    ))?;
    Ok(())
}

fn get_code_for_eval(vm: &Vm, argument: &Object) -> Option<Gc<PrimitiveString>> {
    let mut code = Value::UNDEFINED.0;
    if !call_embedder_hook!(vm, get_code_for_eval, lent_object_into_abi(argument), &raw mut code) {
        return None;
    }
    let code = Value(code);
    assert!(code.is_string(), "the code of an object for eval() is a String");
    Some(code.as_string())
}

fn promise_rejection_tracker(vm: &Vm, promise: Gc<Promise>, operation: RejectionOperation) {
    let operation = match operation {
        RejectionOperation::Reject => JS_PROMISE_REJECTION_OPERATION_REJECT,
        RejectionOperation::Handle => JS_PROMISE_REJECTION_OPERATION_HANDLE,
    };
    call_embedder_hook!(vm, promise_rejection_tracker, promise.as_ptr().cast(), operation);
}

fn call_job_callback(
    vm: &Vm,
    job_callback: Gc<JobCallback>,
    this_value: Value,
    arguments: &[Value],
) -> ThrowCompletionOr<Value> {
    let completion = call_embedder_hook!(
        vm,
        call_job_callback,
        cell_into_abi::<JSJobCallback>(job_callback),
        this_value.0,
        arguments.as_ptr().cast(),
        arguments.len(),
    );
    completion_from_abi(completion)
}

fn enqueue_finalization_registry_cleanup_job(vm: &Vm, finalization_registry: Gc<FinalizationRegistry>) {
    call_embedder_hook!(
        vm,
        enqueue_finalization_registry_cleanup_job,
        finalization_registry.as_ptr().cast()
    );
}

fn enqueue_promise_job(vm: &Vm, job: Gc<HeapFunction>, realm: Option<Gc<Realm>>) {
    call_embedder_hook!(
        vm,
        enqueue_promise_job,
        job.as_ptr().cast(),
        optional_cell_into_abi::<JSRealm>(realm)
    );
}

fn promise_job_queue_is_empty(vm: &Vm) -> bool {
    call_embedder_hook!(vm, promise_job_queue_is_empty)
}

fn make_job_callback(vm: &Vm, callable: Gc<FunctionObject>) -> Gc<JobCallback> {
    let job_callback = call_embedder_hook!(vm, make_job_callback, callable.as_ptr().cast());
    // SAFETY: The embedder returns a JobCallback Record, which its stack keeps alive until it is stored.
    unsafe { cell_from_abi(job_callback) }
}

unsafe extern "C" fn append_import_meta_property(properties: *mut c_void, key: JSPropertyKey, value: JSValue) {
    // SAFETY: The sink's context is the list of properties that get_import_meta_properties() collects, and the
    //         embedder lends a live key.
    let (properties, key) = unsafe {
        (
            &*properties.cast::<MarkedVec<'_, (PropertyKey, Value)>>(),
            clone_lent_property_key_from_abi(key),
        )
    };
    properties.push((key, Value(value)));
}

fn get_import_meta_properties(vm: &Vm, module: Gc<SourceTextModule>) -> MarkedVec<'_, (PropertyKey, Value)> {
    let properties = MarkedVec::new(vm);
    let mut sink = JSImportMetaPropertySink {
        context: core::ptr::from_ref(&properties).cast_mut().cast(),
        append: append_import_meta_property,
    };
    call_embedder_hook!(vm, get_import_meta_properties, module.as_ptr().cast(), &raw mut sink);
    properties
}

fn finalize_import_meta(vm: &Vm, import_meta: Gc<Object>, module: Gc<SourceTextModule>) {
    call_embedder_hook!(
        vm,
        finalize_import_meta,
        import_meta.as_ptr().cast(),
        module.as_ptr().cast()
    );
}

unsafe extern "C" fn append_utf16_string(strings: *mut c_void, code_units: *const u16, length: usize) {
    // SAFETY: The sink's context is the list of strings being collected, and the embedder passes `length` code units.
    let (strings, code_units) = unsafe {
        (
            &mut *strings.cast::<Vec<Utf16String>>(),
            JSUtf16View {
                data: code_units.cast(),
                length_in_code_units: length,
                has_ascii_storage: false,
            }
            .as_view(),
        )
    };
    strings.push(code_units.to_utf16_string());
}

fn get_supported_import_attributes(vm: &Vm) -> Vec<Utf16String> {
    let mut attribute_keys = Vec::new();
    let mut sink = JSStringSink {
        context: (&raw mut attribute_keys).cast(),
        append: Some(append_utf16_string),
    };
    call_embedder_hook!(vm, get_supported_import_attributes, &raw mut sink);
    attribute_keys
}

fn load_imported_module(
    vm: &Vm,
    referrer: ImportedModuleReferrer,
    module_request: &ModuleRequest,
    host_defined: Option<NonNull<c_void>>,
    payload: ImportedModulePayload,
) {
    call_embedder_hook!(
        vm,
        load_imported_module,
        imported_module_referrer_into_abi(referrer),
        module_request_into_abi(module_request),
        host_defined.map_or(core::ptr::null_mut(), NonNull::as_ptr),
        imported_module_payload_into_abi(payload),
    );
}

fn unrecognized_date_string(vm: &Vm, date_string: Utf16View<'_>) {
    call_embedder_hook!(vm, unrecognized_date_string, JSUtf16View::of(date_string));
}

fn resize_array_buffer(vm: &Vm, buffer: &ArrayBuffer, new_byte_length: usize) -> ThrowCompletionOr<HandledByHost> {
    let buffer = core::ptr::from_ref(buffer).cast_mut().cast();
    handled_by_host_from_abi(call_embedder_hook!(vm, resize_array_buffer, buffer, new_byte_length))
}

fn grow_shared_array_buffer(vm: &Vm, buffer: &ArrayBuffer, new_byte_length: usize) -> ThrowCompletionOr<HandledByHost> {
    let buffer = core::ptr::from_ref(buffer).cast_mut().cast();
    handled_by_host_from_abi(call_embedder_hook!(
        vm,
        grow_shared_array_buffer,
        buffer,
        new_byte_length
    ))
}

fn system_utc_epoch_nanoseconds(vm: &Vm, global: &Object) -> SignedBigInteger {
    // NB: Every i64 lies between nsMinInstant and nsMaxInstant, so there is nothing to clamp.
    SignedBigInteger::from(call_embedder_hook!(
        vm,
        system_utc_epoch_nanoseconds,
        lent_object_into_abi(global)
    ))
}

fn on_unimplemented_property_access(vm: &Vm, object: &Object, key: &PropertyKey) {
    call_embedder_hook!(
        vm,
        on_unimplemented_property_access,
        lent_object_into_abi(object),
        lend_property_key_to_abi(key)
    );
}
