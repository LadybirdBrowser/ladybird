/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, OnceCell, RefCell};
use core::ffi::c_void;
use core::ops::ControlFlow;
use core::ptr::NonNull;
use std::collections::HashMap;
use std::collections::HashSet;
use std::collections::VecDeque;
use std::rc::Rc;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use super::interpreter_stack::InterpreterStackMemory;
use crate::build_configuration::VM_STACK_SPACE_LIMIT;
use crate::bytecode::executable::{
    Executable, KeyedPropertyLookupCache, PropertyLookupCache, StaticPropertyLookupCacheSite,
    StaticPropertyLookupCaches,
};
use crate::bytecode::property_access::Strict;
use crate::debugger::Debugger;
use crate::embedding::hooks::Embedder;
use crate::embedding::host::registry::HostClassRegistry;
use crate::gc::capi::{self, GCVisitor};
use crate::gc::class::{GcCell, define_cell};
use crate::gc::foreign::ForeignCellSlot;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap::{Heap, cell_is_dead};
use crate::gc::heap_function::HeapFunction;
use crate::gc::root::{MarkedVec, RootSet};
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak_container::WeakContainer;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::environment::Environment;
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
use crate::layout::function_object::{
    FunctionObject, NATIVE_FUNCTION_TABLE_CAPACITY, NATIVE_FUNCTION_TABLE_INDEX_MASK, NativeFunctionTableEntry,
    NativeFunctionType,
};
use crate::layout::object::Object;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::layout::vm::{ExecutionContextStackEntry, InterpreterStack, VmHead};
use crate::layout_forward::RawNativeFunctionPointer;
use crate::lexical_path;
use crate::runtime::abstract_operations::get_this_environment;
use crate::runtime::agent::AgentRecord;
use crate::runtime::array_buffer::{ArrayBuffer, ZeroFillNewBytes};
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::common_property_names::CommonPropertyNames;
use crate::runtime::completion::{Must, Throw, ThrowCompletionOr};
use crate::runtime::cyclic_module::{CyclicModule, promise_of};
use crate::runtime::declarative_environment::DeclarativeEnvironment;
use crate::runtime::ecmascript_function_object::as_ecmascript_function_object;
use crate::runtime::environment_coordinate::EnvironmentCoordinate;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::finalization_registry::FinalizationRegistry;
use crate::runtime::function_environment::FunctionEnvironment;
use crate::runtime::job_callback::{JobCallback, call_job_callback, make_job_callback};
use crate::runtime::module::{Module, finish_loading_imported_module};
use crate::runtime::module_loading::{ImportedModulePayload, ImportedModuleReferrer};
use crate::runtime::module_request::ModuleRequest;
use crate::runtime::native_javascript_backed_function::NativeJavaScriptBackedFunction;
use crate::runtime::object::IntrinsicAccessor;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::promise::{Promise, PromiseState, RejectionOperation};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::reference::{BaseType, Reference};
use crate::runtime::shape::PrototypeTransitionCache;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::runtime::source_text_module::SourceTextModule;
use crate::runtime::symbol::{self, GlobalSymbolRegistry, Symbol, enumerate_well_known_symbols};
use crate::runtime::synthetic_module::parse_json_module;
use crate::source_code::SourceCode;
use crate::source_range::SourceRange;
use crate::utf16::Utf16View;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HandledByHost {
    Handled,
    Unhandled,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EvalMode {
    Direct,
    Indirect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompilationType {
    DirectEval,
    IndirectEval,
    Function,
    Timer,
}

/// The intrinsic accessors of each object that still has any, by the object's address.
pub type IntrinsicAccessorMap = HashMap<
    usize,
    HashMap<Utf16FlyString, IntrinsicAccessor, foldhash::fast::RandomState>,
    foldhash::fast::RandomState,
>;

/// The properties defined with Object::define_unimplemented_property, by the address of their object.
pub type UnimplementedPropertyMap =
    HashMap<usize, HashSet<Utf16FlyString, foldhash::fast::RandomState>, foldhash::fast::RandomState>;

/// HostEnsureCanAddPrivateElement, which hosts that are web browsers may override.
pub type HostEnsureCanAddPrivateElement = fn(&Vm, &Object) -> ThrowCompletionOr<()>;

/// HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ), which hosts may override.
pub type HostEnqueueFinalizationRegistryCleanupJob = fn(&Vm, Gc<FinalizationRegistry>);

/// HostGetCodeForEval, which hosts may override.
pub type HostGetCodeForEval = fn(&Vm, &Object) -> Option<Gc<PrimitiveString>>;

/// HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString, compilationType,
/// parameterArgs, bodyArg ), which hosts may override.
pub type HostEnsureCanCompileStrings = fn(
    &Vm,
    Gc<Realm>,
    &[Utf16String],
    Utf16View<'_>,
    Utf16View<'_>,
    CompilationType,
    &[Value],
    Value,
) -> ThrowCompletionOr<()>;

/// HostPromiseRejectionTracker ( promise, operation ), which hosts may override.
pub type HostPromiseRejectionTracker = fn(&Vm, Gc<Promise>, RejectionOperation);

/// HostCallJobCallback ( jobCallback, V, argumentsList ), which hosts may override.
pub type HostCallJobCallback = fn(&Vm, Gc<JobCallback>, Value, &[Value]) -> ThrowCompletionOr<Value>;

/// HostEnqueuePromiseJob ( job, realm ), which hosts may override.
pub type HostEnqueuePromiseJob = fn(&Vm, Gc<HeapFunction>, Option<Gc<Realm>>);

/// HostMakeJobCallback ( callback ), which hosts may override.
pub type HostMakeJobCallback = fn(&Vm, Gc<FunctionObject>) -> Gc<JobCallback>;

/// Whether the host's promise job queue is empty, which the synchronous await fast path checks.
pub type HostPromiseJobQueueIsEmpty = fn(&Vm) -> bool;

/// HostSystemUTCEpochNanoseconds ( global ), which hosts may override.
pub type HostSystemUTCEpochNanoseconds = fn(&Vm, &Object) -> SignedBigInteger;

/// VM::host_unrecognized_date_string, which tells the host about a string that date parsing found no date in.
pub type HostUnrecognizedDateString = fn(&Vm, Utf16View<'_>);

/// VM::on_promise_unhandled_rejection and VM::on_promise_rejection_handled, which the default
/// HostPromiseRejectionTracker calls.
pub type PromiseRejectionCallback = fn(&Vm, Gc<Promise>);

/// VM::on_unimplemented_property_access, which [[GetOwnProperty]] calls for a missing property that was defined with
/// Object::define_unimplemented_property.
pub type OnUnimplementedPropertyAccess = fn(&Vm, &Object, &PropertyKey);

/// How a VM is set up, which is fixed once it is created.
#[derive(Clone, Copy, Debug, Default)]
pub struct VmOptions {
    /// Makes the VM's heap the one GC::Heap::the() returns, so that the embedder's C++ cells are allocated from it.
    pub become_process_default_heap: bool,
    /// Backs SharedArrayBuffers with shared memory, which other processes can map.
    pub shared_memory_shared_array_buffers: bool,
}

/// An execution context stack that VM::save_execution_context_stack() set aside, with the TypeErrorRealmScope that
/// was active on it.
struct SavedExecutionContextStack {
    storage: Vec<ExecutionContextStackEntry>,
    length: usize,
    running_execution_context: *mut ExecutionContext,
    type_error_realm_override: Option<Gc<Realm>>,
    type_error_realm_override_depth: usize,
}

/// VM::for_each_execution_context_top_to_bottom() over any execution context stack, the live one or a saved one.
/// `stack_entry` returns the context at an index of the stack and the context that was running when it was pushed, or
/// None past the end of the stack. The walk asks for each entry when it gets there, so that a walk of the live stack
/// borrows it only for that moment.
fn for_each_execution_context_top_to_bottom_of(
    stack_length: usize,
    stack_entry: impl Fn(usize) -> Option<(NonNull<ExecutionContext>, *mut ExecutionContext)>,
    running_execution_context: *mut ExecutionContext,
    mut callback: impl FnMut(&ExecutionContext) -> ControlFlow<()>,
) {
    let mut stack_index = stack_length;
    let Some(mut context) = NonNull::new(running_execution_context) else {
        while stack_index > 0 {
            stack_index -= 1;
            let Some((context, _)) = stack_entry(stack_index) else {
                return;
            };
            // SAFETY: Contexts on the stack are live.
            if callback(unsafe { context.as_ref() }).is_break() {
                return;
            }
        }
        return;
    };
    loop {
        // SAFETY: Every context reachable from the running one is live.
        let context_ref = unsafe { context.as_ref() };
        if callback(context_ref).is_break() {
            return;
        }
        let next = match stack_index.checked_sub(1).and_then(&stack_entry) {
            Some((base_context, previous_running_context)) if base_context == context => {
                stack_index -= 1;
                previous_running_context
            }
            _ => context_ref.caller_frame.get(),
        };
        let Some(next) = NonNull::new(next) else {
            return;
        };
        context = next;
    }
}

pub(crate) fn default_host_promise_rejection_tracker(vm: &Vm, promise: Gc<Promise>, operation: RejectionOperation) {
    vm.promise_rejection_tracker(promise, operation);
}

pub(crate) fn default_host_enqueue_promise_job(vm: &Vm, job: Gc<HeapFunction>, realm: Option<Gc<Realm>>) {
    vm.enqueue_promise_job(job, realm);
}

pub(crate) fn default_host_promise_job_queue_is_empty(vm: &Vm) -> bool {
    vm.job_queues().promise_jobs.borrow().is_empty()
}

// 2.3.1 HostSystemUTCEpochNanoseconds ( global ), https://tc39.es/proposal-temporal/#sec-hostsystemutcepochnanoseconds
pub(crate) fn default_host_system_utc_epoch_nanoseconds(_: &Vm, _: &Object) -> SignedBigInteger {
    use crate::runtime::temporal::instant::{NANOSECONDS_MAX_INSTANT, NANOSECONDS_MIN_INSTANT};

    // 1. Let ns be the approximate current UTC date and time, in nanoseconds since the epoch.
    // NB: Like AK::UnixDateTime::now().nanoseconds_since_epoch(), this saturates to the i64 range.
    let since_epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or_else(
            |error| -(error.duration().as_nanos() as i128),
            |duration| duration.as_nanos() as i128,
        );
    let nanoseconds = SignedBigInteger::from(since_epoch.clamp(i128::from(i64::MIN), i128::from(i64::MAX)));

    // 2. Return the result of clamping ns between nsMinInstant and nsMaxInstant.
    nanoseconds.clamp(NANOSECONDS_MIN_INSTANT.clone(), NANOSECONDS_MAX_INSTANT.clone())
}

/// The VM's queues of promise jobs and finalization registry cleanup jobs, in a cell the VM roots.
#[repr(C)]
#[derive(Trace)]
pub struct JobQueues {
    header: CellHeader,
    promise_jobs: GcRefCell<VecDeque<Gc<HeapFunction>>>,
    finalization_registry_cleanup_jobs: GcRefCell<Vec<Gc<FinalizationRegistry>>>,
}

define_cell!(JobQueues, Other);

// 16.2.1.10 HostLoadImportedModule ( referrer, moduleRequest, hostDefined, payload ), https://tc39.es/ecma262/#sec-HostLoadImportedModule
/// HostLoadImportedModule, whose hostDefined is the cell LoadRequestedModules was given, or none for EMPTY.
pub type HostLoadImportedModule =
    fn(&Vm, ImportedModuleReferrer, &ModuleRequest, Option<NonNull<c_void>>, ImportedModulePayload);

/// HostGetImportMetaProperties, which returns the properties import.meta starts with.
pub type HostGetImportMetaProperties =
    for<'vm> fn(&'vm Vm, Gc<SourceTextModule>) -> MarkedVec<'vm, (PropertyKey, Value)>;

/// HostFinalizeImportMeta.
pub type HostFinalizeImportMeta = fn(&Vm, Gc<Object>, Gc<SourceTextModule>);

/// HostGetSupportedImportAttributes.
pub type HostGetSupportedImportAttributes = fn(&Vm) -> Vec<Utf16String>;

pub(crate) fn default_host_get_import_meta_properties(
    vm: &Vm,
    _: Gc<SourceTextModule>,
) -> MarkedVec<'_, (PropertyKey, Value)> {
    MarkedVec::new(vm)
}

pub(crate) fn default_host_finalize_import_meta(_: &Vm, _: Gc<Object>, _: Gc<SourceTextModule>) {}

pub(crate) fn default_host_get_supported_import_attributes(_: &Vm) -> Vec<Utf16String> {
    vec![Utf16String::from_utf8("type")]
}

/// A module the VM loaded.
#[derive(Clone, Trace)]
struct StoredModule {
    referrer: ImportedModuleReferrer,
    filename: String,
    module_type: String,
    module: Gc<Module>,
    has_once_started_linking: bool,
}

/// The modules the VM loaded, in a cell the VM roots.
#[repr(C)]
#[derive(Trace)]
pub struct LoadedModules {
    header: CellHeader,
    stored_modules: GcRefCell<Vec<StoredModule>>,
}

define_cell!(LoadedModules, Other);

/// HostResizeArrayBuffer, which hosts may override.
pub type HostResizeArrayBuffer = fn(&Vm, &ArrayBuffer, usize) -> ThrowCompletionOr<HandledByHost>;

/// HostGrowSharedArrayBuffer, which hosts may override.
pub type HostGrowSharedArrayBuffer = fn(&Vm, &ArrayBuffer, usize) -> ThrowCompletionOr<HandledByHost>;

// 25.1.3.8 HostResizeArrayBuffer ( buffer, newByteLength ), https://tc39.es/ecma262/#sec-hostresizearraybuffer
pub(crate) fn default_host_resize_array_buffer(
    vm: &Vm,
    buffer: &ArrayBuffer,
    new_byte_length: usize,
) -> ThrowCompletionOr<HandledByHost> {
    // The host-defined abstract operation HostResizeArrayBuffer takes arguments buffer (an ArrayBuffer) and
    // newByteLength (a non-negative integer) and returns either a normal completion containing either handled or
    // unhandled, or a throw completion. It gives the host an opportunity to perform implementation-defined resizing
    // of buffer. If the host chooses not to handle resizing of buffer, it may return unhandled for the default behaviour.

    // The implementation of HostResizeArrayBuffer must conform to the following requirements:
    // - The abstract operation does not detach buffer.
    // - If the abstract operation completes normally with handled, buffer.[[ArrayBufferByteLength]] is newByteLength.

    // The default implementation of HostResizeArrayBuffer is to return NormalCompletion(unhandled).

    if buffer.try_resize(vm, new_byte_length, ZeroFillNewBytes::Yes).is_err() {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::NotEnoughMemoryToAllocate,
            &[&new_byte_length],
        );
    }

    Ok(HandledByHost::Handled)
}

// 25.2.2.4 HostGrowSharedArrayBuffer ( buffer, newByteLength ), https://tc39.es/ecma262/#sec-hostgrowsharedarraybuffer
#[allow(clippy::unnecessary_wraps, reason = "the hook's type lets other hosts throw")]
pub(crate) fn default_host_grow_shared_array_buffer(
    _: &Vm,
    _: &ArrayBuffer,
    _: usize,
) -> ThrowCompletionOr<HandledByHost> {
    // The host-defined abstract operation HostGrowSharedArrayBuffer takes arguments buffer (a SharedArrayBuffer)
    // and newByteLength (a non-negative integer) and returns either a normal completion containing either handled
    // or unhandled, or a throw completion. It gives the host an opportunity to perform implementation-defined
    // growing of buffer. If the host chooses not to handle growing of buffer, it may return unhandled for the default behaviour.

    // The implementation of HostGrowSharedArrayBuffer must conform to the following requirements:
    // - If the abstract operation does not complete normally with unhandled, and newByteLength < the current byte length of the buffer or newByteLength > buffer.[[ArrayBufferMaxByteLength]], throw a RangeError exception.
    // - Let AR be the Agent Record of the surrounding agent. Let isLittleEndian be AR.[[LittleEndian]]. If the abstract operation completes normally with handled, a WriteSharedMemory or ReadModifyWriteSharedMemory event whose [[Order]] is seq-cst, [[Payload]] is NumericToRawBytes(biguint64, newByteLength, isLittleEndian), [[Block]] is buffer.[[ArrayBufferByteLengthData]], [[ByteIndex]] is 0, and [[ElementSize]] is 8 is added to the surrounding agent's candidate execution such that racing calls to SharedArrayBuffer.prototype.grow ( newLength ) are not "lost", i.e. silently do nothing.

    // The default implementation of HostGrowSharedArrayBuffer is to return NormalCompletion(unhandled).
    Ok(HandledByHost::Unhandled)
}

// 1 HostGetCodeForEval ( argument ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostgetcodeforeval
pub(crate) fn default_host_get_code_for_eval(_: &Vm, _: &Object) -> Option<Gc<PrimitiveString>> {
    // The host-defined abstract operation HostGetCodeForEval takes argument argument (an Object) and returns a
    // String or NO-CODE. It allows host environments to return a String of code from argument to be used by eval,
    // rather than eval returning argument.
    //
    // argument represents the Object to be checked for code.
    //
    // The default implementation of HostGetCodeForEval is to return NO-CODE.
    None
}

// 2 HostEnsureCanCompileStrings ( calleeRealm, parameterStrings, bodyString, codeString, compilationType, parameterArgs, bodyArg ), https://tc39.es/proposal-dynamic-code-brand-checks/#sec-hostensurecancompilestrings
#[allow(clippy::too_many_arguments, reason = "the hook takes the spec's arguments")]
#[allow(clippy::unnecessary_wraps, reason = "the hook's type lets other hosts throw")]
pub(crate) fn default_host_ensure_can_compile_strings(
    _: &Vm,
    _: Gc<Realm>,
    _: &[Utf16String],
    _: Utf16View<'_>,
    _: Utf16View<'_>,
    _: CompilationType,
    _: &[Value],
    _: Value,
) -> ThrowCompletionOr<()> {
    // The host-defined abstract operation HostEnsureCanCompileStrings takes arguments calleeRealm (a Realm Record),
    // parameterStrings (a List of Strings), bodyString (a String), and direct (a Boolean) and returns either a normal
    // completion containing unused or a throw completion.
    //
    // It allows host environments to block certain ECMAScript functions which allow developers to compile strings into ECMAScript code.
    // An implementation of HostEnsureCanCompileStrings must conform to the following requirements:
    //   - If the returned Completion Record is a normal completion, it must be a normal completion containing unused.
    // The default implementation of HostEnsureCanCompileStrings is to return NormalCompletion(unused).
    Ok(())
}

#[allow(clippy::unnecessary_wraps, reason = "the hook's type lets other hosts throw")]
pub(crate) fn default_host_ensure_can_add_private_element(_: &Vm, _: &Object) -> ThrowCompletionOr<()> {
    // The host-defined abstract operation HostEnsureCanAddPrivateElement takes argument O (an Object)
    // and returns either a normal completion containing unused or a throw completion.
    // It allows host environments to prevent the addition of private elements to particular host-defined exotic objects.
    // An implementation of HostEnsureCanAddPrivateElement must conform to the following requirements:
    // - If O is not a host-defined exotic object, this abstract operation must return NormalCompletion(unused) and perform no other steps.
    // - Any two calls of this abstract operation with the same argument must return the same kind of Completion Record.
    // The default implementation of HostEnsureCanAddPrivateElement is to return NormalCompletion(unused).
    Ok(())

    // This abstract operation is only invoked by ECMAScript hosts that are web browsers.
    // NOTE: Since LibJS has no way of knowing whether the current environment is a browser we always
    //       call HostEnsureCanAddPrivateElement when needed.
}

pub(crate) fn default_host_unrecognized_date_string(_: &Vm, _: Utf16View<'_>) {}

// 9.10.4.1 HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ), https://tc39.es/ecma262/#sec-host-cleanup-finalization-registry
pub(crate) fn default_host_enqueue_finalization_registry_cleanup_job(
    vm: &Vm,
    finalization_registry: Gc<FinalizationRegistry>,
) {
    vm.enqueue_finalization_registry_cleanup_job(finalization_registry);
}

pub const STRING_TO_ATOM_CACHE_SIZE: usize = 256;
pub const FLY_STRING_CACHE_SIZE: usize = 1024;
pub const NUMERIC_STRING_CACHE_SIZE: usize = 1000;
pub const LARGE_NUMERIC_STRING_CACHE_SIZE: usize = 1024;
const SINGLE_ASCII_CHARACTER_STRING_COUNT: usize = 128;

/// Remembers the property key of a string that is not stored as a fly string.
#[derive(Default)]
pub struct StringToAtomCacheEntry {
    pub string: Option<Gc<PrimitiveString>>,
    pub atom: Option<Utf16FlyString>,
}

#[derive(Trace)]
pub struct NumericStringCacheEntry {
    pub number: Cell<u64>,
    pub string: Cell<Option<Gc<PrimitiveString>>>,
}

#[derive(Trace)]
#[allow(non_snake_case)]
pub struct CachedStrings {
    pub number: Gc<PrimitiveString>,
    pub undefined: Gc<PrimitiveString>,
    pub object: Gc<PrimitiveString>,
    pub string: Gc<PrimitiveString>,
    pub symbol: Gc<PrimitiveString>,
    pub boolean: Gc<PrimitiveString>,
    pub bigint: Gc<PrimitiveString>,
    pub function: Gc<PrimitiveString>,
    pub object_Object: Gc<PrimitiveString>,
}

macro_rules! define_well_known_symbols {
    ($(($symbol_name:ident, $snake_name:ident),)*) => {
        #[derive(Trace)]
        pub struct WellKnownSymbols {
            $(pub $snake_name: Gc<Symbol>,)*
        }

        impl WellKnownSymbols {
            fn create(vm: &Vm) -> Self {
                Self {
                    $($snake_name: Symbol::create(
                        vm,
                        Some(Utf16String::from_utf8(concat!("Symbol.", stringify!($symbol_name)))),
                        symbol::Kind::Unique,
                    ),)*
                }
            }
        }
    };
}

enumerate_well_known_symbols!(define_well_known_symbols);

/// The virtual machine the interpreter runs on. The interpreter reads and writes the head directly.
#[repr(C)]
pub struct Vm {
    pub head: VmHead,
    heap: OnceCell<Heap>,
    _interpreter_stack_memory: InterpreterStackMemory,
    /// The storage of the execution context stack, whose length is its capacity. The head points into it and knows
    /// which of its entries are on the stack.
    execution_context_stack_storage: RefCell<Vec<ExecutionContextStackEntry>>,
    /// All NATIVE_FUNCTION_TABLE_CAPACITY entries, of which the first native_function_count are registered. The rest
    /// have no function.
    native_function_table: RefCell<Box<[NativeFunctionTableEntry]>>,
    native_function_count: Cell<u32>,
    /// The index of each function in the native function table, by its address and type.
    native_function_indices: RefCell<HashMap<(usize, u32), u32>>,
    roots: RootSet,

    pub names: CommonPropertyNames,
    /// The strings in these two caches are weak: the sweep callback drops the ones that die.
    string_to_atom_cache: RefCell<Box<[StringToAtomCacheEntry; STRING_TO_ATOM_CACHE_SIZE]>>,
    fly_string_cache: Box<[Cell<Option<Gc<PrimitiveString>>>; FLY_STRING_CACHE_SIZE]>,
    numeric_string_cache: Box<[Cell<Option<Gc<PrimitiveString>>>; NUMERIC_STRING_CACHE_SIZE]>,
    large_numeric_string_cache: Box<[NumericStringCacheEntry; LARGE_NUMERIC_STRING_CACHE_SIZE]>,
    /// The interned PrimitiveString of each Utf16FlyString, by its raw identity, see PrimitiveString::is_interned().
    /// The strings are weak: the sweep callback drops the ones that die.
    interned_strings: RefCell<HashMap<usize, Gc<PrimitiveString>, foldhash::fast::RandomState>>,
    empty_string: OnceCell<Gc<PrimitiveString>>,
    cached_strings: OnceCell<CachedStrings>,
    single_ascii_character_strings: OnceCell<[Gc<PrimitiveString>; SINGLE_ASCII_CHARACTER_STRING_COUNT]>,
    well_known_symbols: OnceCell<WellKnownSymbols>,
    global_symbol_registry: OnceCell<Gc<GlobalSymbolRegistry>>,

    /// The executables whose inline caches the sweep callback prunes. The list is weak: the callback drops the
    /// executables that die.
    executables: RefCell<Vec<Gc<Executable>>>,
    static_property_lookup_caches: StaticPropertyLookupCaches,
    keyed_property_lookup_cache: KeyedPropertyLookupCache,
    prototype_transition_cache: Box<PrototypeTransitionCache>,
    /// Like the keyed property lookup cache, how string-keyed PutByValue instructions whose own caches gave up put
    /// properties: the writable own data properties they change and the properties they add, by shape and name (see
    /// put_by_value_with_keyed_store_cache()).
    keyed_property_store_cache: KeyedPropertyLookupCache,
    /// An empty cache that PutByValue fills for one put at a time, to find out how to remember it in the keyed
    /// property store cache.
    keyed_property_store_scratch_cache: PropertyLookupCache,
    host_ensure_can_add_private_element: Cell<HostEnsureCanAddPrivateElement>,
    host_get_code_for_eval: Cell<HostGetCodeForEval>,
    host_ensure_can_compile_strings: Cell<HostEnsureCanCompileStrings>,
    host_promise_rejection_tracker: Cell<HostPromiseRejectionTracker>,
    host_call_job_callback: Cell<HostCallJobCallback>,
    host_enqueue_finalization_registry_cleanup_job: Cell<HostEnqueueFinalizationRegistryCleanupJob>,
    host_enqueue_promise_job: Cell<HostEnqueuePromiseJob>,
    host_make_job_callback: Cell<HostMakeJobCallback>,
    host_promise_job_queue_is_empty: Cell<HostPromiseJobQueueIsEmpty>,
    host_system_utc_epoch_nanoseconds: Cell<HostSystemUTCEpochNanoseconds>,
    host_unrecognized_date_string: Cell<HostUnrecognizedDateString>,
    on_promise_unhandled_rejection: Cell<Option<PromiseRejectionCallback>>,
    on_promise_rejection_handled: Cell<Option<PromiseRejectionCallback>>,
    job_queues: OnceCell<Gc<JobQueues>>,
    /// How many run_executable() calls are running: the outermost one runs the promise jobs its code queued.
    run_executable_depth: Cell<u32>,
    /// The cells that hold others weakly, LibGC's list of weak containers. The list is weak: the sweep callback drops
    /// the containers that die, and has the others forget the cells that died.
    weak_containers: RefCell<Vec<WeakContainer>>,
    /// The finalization registries whose targets died in the collection in progress, which are handed to
    /// HostEnqueueFinalizationRegistryCleanupJob once it is over.
    finalization_registries_with_dead_cells: RefCell<Vec<Gc<FinalizationRegistry>>>,
    host_resize_array_buffer: Cell<HostResizeArrayBuffer>,
    host_grow_shared_array_buffer: Cell<HostGrowSharedArrayBuffer>,
    host_load_imported_module: Cell<HostLoadImportedModule>,
    host_get_import_meta_properties: Cell<HostGetImportMetaProperties>,
    host_finalize_import_meta: Cell<HostFinalizeImportMeta>,
    host_get_supported_import_attributes: Cell<HostGetSupportedImportAttributes>,
    dynamic_imports_allowed: Cell<bool>,
    loaded_modules: OnceCell<Gc<LoadedModules>>,
    module_execution_depth: Cell<u32>,
    module_async_evaluation_count: Cell<u64>, // [[ModuleAsyncEvaluationCount]]
    /// The id the next PrivateEnvironment gives its names. It starts at one such that 0 can be invalid / default
    /// initialized.
    next_private_environment_id: Cell<u64>,
    /// The properties defined with Object::define_intrinsic_accessor that have not been read yet, by the address of
    /// their object. The objects are weak: the sweep callback forgets the ones that die.
    intrinsic_accessors: RefCell<IntrinsicAccessorMap>,
    /// The debugger, which the head points to while it is attached.
    debugger: RefCell<Option<Rc<Debugger>>>,
    /// The properties defined with Object::define_unimplemented_property. The objects are weak: the sweep callback
    /// forgets the ones that die.
    unimplemented_properties: RefCell<UnimplementedPropertyMap>,
    on_unimplemented_property_access: Cell<Option<OnUnimplementedPropertyAccess>>,

    options: VmOptions,
    embedder: Cell<Option<Embedder>>,
    agent: Cell<AgentRecord>,
    saved_execution_context_stacks: RefCell<Vec<SavedExecutionContextStack>>,
    host_classes: RefCell<HostClassRegistry>,
}

const _: () = assert!(core::mem::offset_of!(Vm, head) == 0);

impl Vm {
    pub fn create() -> Box<Vm> {
        Self::create_with(VmOptions::default())
    }

    pub fn create_with(options: VmOptions) -> Box<Vm> {
        let mut storage = Box::<Vm>::new_uninit();
        // SAFETY: The box is storage for a Vm, and the VM stays in it until the box drops it.
        unsafe { Self::create_at(storage.as_mut_ptr(), options) };
        // SAFETY: create_at() constructed the VM in the box.
        unsafe { storage.assume_init() }
    }

    /// Constructs a VM in `storage`, its final address: the heap calls back into the VM there, and cells such as
    /// WeakRefs point into it.
    ///
    /// # Safety
    ///
    /// `storage` must be valid for writes of a Vm and aligned for one. The VM must stay there until it is dropped in
    /// place, which only the thread that constructed it may do.
    pub unsafe fn create_at(storage: *mut Vm, options: VmOptions) {
        // SAFETY: Initializes the region cell pointers are relative to.
        let heap_region_base = unsafe { capi::gc_heap_region_base() };
        let primitive_storage_cage_base = crate::runtime::array_buffer::primitive_storage_cage_base();
        let interpreter_stack_memory = InterpreterStackMemory::allocate();
        // SAFETY: An all-zero NativeFunctionTableEntry is valid: no function and the first function type.
        let native_function_table = unsafe {
            Box::<[NativeFunctionTableEntry]>::new_zeroed_slice(NATIVE_FUNCTION_TABLE_CAPACITY).assume_init()
        };
        let mut execution_context_stack_storage = Vec::new();
        let vm = Vm {
            head: VmHead {
                running_execution_context: Cell::new(core::ptr::null_mut()),
                interpreter_stack: interpreter_stack_memory.initial_state(),
                stack_base: Cell::new(0),
                execution_generation: Cell::new(0),
                primitive_storage_cage_base: Cell::new(primitive_storage_cage_base),
                heap_region_base: Cell::new(heap_region_base),
                native_function_table_data: Cell::new(native_function_table.as_ptr()),
                debugger: Cell::new(core::ptr::null_mut()),
                execution_context_stack_entries: Cell::new(Vec::<ExecutionContextStackEntry>::as_mut_ptr(
                    &mut execution_context_stack_storage,
                )),
                execution_context_stack_length: Cell::new(0),
                execution_context_stack_capacity: Cell::new(0),
                type_error_realm_override: Cell::new(None),
                type_error_realm_override_depth: Cell::new(0),
                keyed_property_lookup_cache_entries: Cell::new(core::ptr::null()),
            },
            heap: OnceCell::new(),
            _interpreter_stack_memory: interpreter_stack_memory,
            execution_context_stack_storage: RefCell::new(execution_context_stack_storage),
            native_function_table: RefCell::new(native_function_table),
            native_function_count: Cell::new(0),
            native_function_indices: RefCell::new(HashMap::new()),
            roots: RootSet::default(),
            names: CommonPropertyNames::new(),
            string_to_atom_cache: RefCell::new(Box::new(core::array::from_fn(|_| StringToAtomCacheEntry::default()))),
            fly_string_cache: Box::new([const { Cell::new(None) }; FLY_STRING_CACHE_SIZE]),
            numeric_string_cache: Box::new([const { Cell::new(None) }; NUMERIC_STRING_CACHE_SIZE]),
            large_numeric_string_cache: Box::new(
                [const {
                    NumericStringCacheEntry {
                        number: Cell::new(0),
                        string: Cell::new(None),
                    }
                }; LARGE_NUMERIC_STRING_CACHE_SIZE],
            ),
            interned_strings: RefCell::default(),
            empty_string: OnceCell::new(),
            cached_strings: OnceCell::new(),
            single_ascii_character_strings: OnceCell::new(),
            well_known_symbols: OnceCell::new(),
            global_symbol_registry: OnceCell::new(),
            executables: RefCell::new(Vec::new()),
            static_property_lookup_caches: StaticPropertyLookupCaches::new(),
            keyed_property_lookup_cache: KeyedPropertyLookupCache::new(),
            prototype_transition_cache: Box::new(PrototypeTransitionCache::new()),
            keyed_property_store_cache: KeyedPropertyLookupCache::new(),
            keyed_property_store_scratch_cache: PropertyLookupCache::new(),
            host_ensure_can_add_private_element: Cell::new(default_host_ensure_can_add_private_element),
            host_get_code_for_eval: Cell::new(default_host_get_code_for_eval),
            host_ensure_can_compile_strings: Cell::new(default_host_ensure_can_compile_strings),
            host_promise_rejection_tracker: Cell::new(default_host_promise_rejection_tracker),
            host_call_job_callback: Cell::new(call_job_callback),
            host_enqueue_finalization_registry_cleanup_job: Cell::new(
                default_host_enqueue_finalization_registry_cleanup_job,
            ),
            host_enqueue_promise_job: Cell::new(default_host_enqueue_promise_job),
            host_make_job_callback: Cell::new(make_job_callback),
            host_promise_job_queue_is_empty: Cell::new(default_host_promise_job_queue_is_empty),
            host_system_utc_epoch_nanoseconds: Cell::new(default_host_system_utc_epoch_nanoseconds),
            host_unrecognized_date_string: Cell::new(default_host_unrecognized_date_string),
            on_promise_unhandled_rejection: Cell::new(None),
            on_promise_rejection_handled: Cell::new(None),
            job_queues: OnceCell::new(),
            run_executable_depth: Cell::new(0),
            weak_containers: RefCell::new(Vec::new()),
            finalization_registries_with_dead_cells: RefCell::new(Vec::new()),
            host_resize_array_buffer: Cell::new(default_host_resize_array_buffer),
            host_grow_shared_array_buffer: Cell::new(default_host_grow_shared_array_buffer),
            host_load_imported_module: Cell::new(Vm::load_imported_module),
            host_get_import_meta_properties: Cell::new(default_host_get_import_meta_properties),
            host_finalize_import_meta: Cell::new(default_host_finalize_import_meta),
            host_get_supported_import_attributes: Cell::new(default_host_get_supported_import_attributes),
            dynamic_imports_allowed: Cell::new(false),
            loaded_modules: OnceCell::new(),
            module_execution_depth: Cell::new(0),
            module_async_evaluation_count: Cell::new(0),
            next_private_environment_id: Cell::new(1),
            intrinsic_accessors: RefCell::new(HashMap::default()),
            debugger: RefCell::new(None),
            unimplemented_properties: RefCell::new(HashMap::default()),
            on_unimplemented_property_access: Cell::new(None),
            options,
            embedder: Cell::new(None),
            agent: Cell::new(AgentRecord::default()),
            saved_execution_context_stacks: RefCell::new(Vec::new()),
            host_classes: RefCell::new(HashMap::default()),
        };
        // SAFETY: The caller provides storage for a Vm.
        unsafe { storage.write(vm) };
        // SAFETY: The VM was just written there.
        let vm = unsafe { &*storage };
        vm.head
            .keyed_property_lookup_cache_entries
            .set(vm.keyed_property_lookup_cache.entries_for_interpreter());
        let context = storage.cast();
        // SAFETY: The VM stays at this address until it is dropped, and it destroys the heap before anything else.
        let heap = unsafe { Heap::new(gather_roots, context, options.become_process_default_heap) };
        // SAFETY: As above.
        unsafe { heap.register_sweep_callback(sweep, context) };
        vm.head.stack_base.set(heap.stack_bounds().0);
        if vm.heap.set(heap).is_err() {
            unreachable!("the heap is created once");
        }
        vm.allocate_preallocated_strings_and_symbols();
    }

    fn allocate_preallocated_strings_and_symbols(&self) {
        // NB: The empty and single ASCII character strings are interned without being in the table of interned
        //     strings, since every way to create one of them returns these.
        let allocate_interned_string = |string: &str| {
            let string = self
                .heap()
                .allocate(PrimitiveString::new(Utf16String::from_utf8(string)));
            string.set_interned();
            string
        };

        let _ = self.empty_string.set(allocate_interned_string(""));

        let _ = self
            .single_ascii_character_strings
            .set(core::array::from_fn(|character| {
                let character = [character as u8];
                allocate_interned_string(core::str::from_utf8(&character).expect("ASCII is UTF-8"))
            }));

        let create_interned = |string: &str| PrimitiveString::create_interned(self, &Utf16FlyString::from_utf8(string));
        let _ = self.cached_strings.set(CachedStrings {
            number: create_interned("number"),
            undefined: create_interned("undefined"),
            object: create_interned("object"),
            string: create_interned("string"),
            symbol: create_interned("symbol"),
            boolean: create_interned("boolean"),
            bigint: create_interned("bigint"),
            function: create_interned("function"),
            object_Object: create_interned("[object Object]"),
        });

        let _ = self.well_known_symbols.set(WellKnownSymbols::create(self));
        let _ = self.global_symbol_registry.set(GlobalSymbolRegistry::create(self));
        let _ = self.job_queues.set(self.heap().allocate(JobQueues {
            header: CellHeader::for_class(JobQueues::CLASS),
            promise_jobs: GcRefCell::new(VecDeque::new()),
            finalization_registry_cleanup_jobs: GcRefCell::new(Vec::new()),
        }));
        let _ = self.loaded_modules.set(self.heap().allocate(LoadedModules {
            header: CellHeader::for_class(LoadedModules::CLASS),
            stored_modules: GcRefCell::new(Vec::new()),
        }));
    }

    pub fn heap(&self) -> &Heap {
        self.heap.get().expect("the VM has a heap")
    }

    pub fn interpreter_stack(&self) -> &InterpreterStack {
        &self.head.interpreter_stack
    }

    pub fn running_execution_context(&self) -> Option<NonNull<ExecutionContext>> {
        NonNull::new(self.head.running_execution_context.get())
    }

    /// The Realm of the running execution context.
    pub fn current_realm(&self) -> Option<Gc<Realm>> {
        let context = self.running_execution_context()?;
        // SAFETY: The running execution context is live.
        unsafe { context.as_ref() }.realm.get()
    }

    /// Whether so little of the native stack is left that running more JavaScript could overflow it.
    pub fn did_reach_stack_space_limit(&self) -> bool {
        let marker = 0u8;
        let current = core::ptr::from_ref(&marker) as usize;
        current.saturating_sub(self.head.stack_base.get()) < VM_STACK_SPACE_LIMIT as usize
    }

    pub fn push_execution_context(&self, context: NonNull<ExecutionContext>) {
        // SAFETY: The caller passes a live context.
        let context_ref = unsafe { context.as_ref() };
        context_ref.caller_frame.set(core::ptr::null_mut());
        context_ref.caller_return_pc.set(0);
        context_ref.caller_dst_raw.set(0);
        context_ref.caller_is_construct.set(false);
        let length = self.head.execution_context_stack_length.get();
        if length == self.head.execution_context_stack_capacity.get() {
            self.grow_execution_context_stack();
        }
        // SAFETY: The storage has room for more entries than `length`.
        let entry = unsafe { &*self.head.execution_context_stack_entries.get().add(length) };
        entry.execution_context.set(context.as_ptr());
        entry
            .previous_running_execution_context
            .set(self.head.running_execution_context.get());
        self.head.execution_context_stack_length.set(length + 1);
        self.head.running_execution_context.set(context.as_ptr());
    }

    pub fn pop_execution_context(&self) -> NonNull<ExecutionContext> {
        let length = self
            .head
            .execution_context_stack_length
            .get()
            .checked_sub(1)
            .expect("a context to pop");
        // SAFETY: The entry below the length is on the stack.
        let entry = unsafe { &*self.head.execution_context_stack_entries.get().add(length) };
        self.head.execution_context_stack_length.set(length);
        self.head
            .running_execution_context
            .set(entry.previous_running_execution_context.get());
        NonNull::new(entry.execution_context.get()).expect("every entry on the stack has its context")
    }

    #[cold]
    fn grow_execution_context_stack(&self) {
        let mut storage = self.execution_context_stack_storage.borrow_mut();
        let capacity = (storage.len() * 2).max(16);
        storage.resize_with(capacity, || ExecutionContextStackEntry {
            execution_context: Cell::new(core::ptr::null_mut()),
            previous_running_execution_context: Cell::new(core::ptr::null_mut()),
        });
        self.head.execution_context_stack_entries.set(storage.as_mut_ptr());
        self.head.execution_context_stack_capacity.set(capacity);
    }

    /// The context at `index` of the execution context stack and the context that was running when it was pushed, or
    /// None past the top of the stack.
    fn execution_context_stack_entry(
        &self,
        index: usize,
    ) -> Option<(NonNull<ExecutionContext>, *mut ExecutionContext)> {
        if index >= self.head.execution_context_stack_length.get() {
            return None;
        }
        // SAFETY: The entries below the length are on the stack.
        let entry = unsafe { &*self.head.execution_context_stack_entries.get().add(index) };
        Some((
            NonNull::new(entry.execution_context.get()).expect("every entry on the stack has its context"),
            entry.previous_running_execution_context.get(),
        ))
    }

    /// Hands the head the storage of an execution context stack with `length` entries on it.
    fn install_execution_context_stack(&self, mut storage: Vec<ExecutionContextStackEntry>, length: usize) {
        assert!(length <= storage.len());
        self.head.execution_context_stack_entries.set(storage.as_mut_ptr());
        self.head.execution_context_stack_length.set(length);
        self.head.execution_context_stack_capacity.set(storage.len());
        *self.execution_context_stack_storage.borrow_mut() = storage;
    }

    /// Calls `callback` with each live execution context from the running one down, until it breaks, following both
    /// the frames the interpreter links through caller_frame and the contexts pushed onto the execution context stack.
    /// No borrow of the stack is held while `callback` runs, so it may run JavaScript, as long as it leaves the stack
    /// as it found it.
    pub fn for_each_execution_context_top_to_bottom(&self, callback: impl FnMut(&ExecutionContext) -> ControlFlow<()>) {
        let stack_length = self.execution_context_stack_size();
        for_each_execution_context_top_to_bottom_of(
            stack_length,
            |index| self.execution_context_stack_entry(index),
            self.head.running_execution_context.get(),
            callback,
        );
    }

    /// VM::last_execution_context_matching(): the topmost execution context that `predicate` accepts, in the order of
    /// for_each_execution_context_top_to_bottom().
    pub fn last_execution_context_matching(
        &self,
        mut predicate: impl FnMut(NonNull<ExecutionContext>) -> bool,
    ) -> Option<NonNull<ExecutionContext>> {
        let mut matching_execution_context = None;
        self.for_each_execution_context_top_to_bottom(|execution_context| {
            let execution_context = NonNull::from(execution_context);
            if !predicate(execution_context) {
                return ControlFlow::Continue(());
            }
            matching_execution_context = Some(execution_context);
            ControlFlow::Break(())
        });
        matching_execution_context
    }

    /// The number of contexts pushed onto the execution context stack, VM::execution_context_stack().size(). Frames
    /// the interpreter links through caller_frame are not on it.
    pub fn execution_context_stack_size(&self) -> usize {
        self.head.execution_context_stack_length.get()
    }

    /// VM::save_execution_context_stack(): sets the execution context stack aside, with the TypeErrorRealmScope that
    /// is active on it, and leaves an empty stack with nothing running. The garbage collector keeps tracing the saved
    /// stack until restore_execution_context_stack() brings it back.
    pub fn save_execution_context_stack(&self) {
        let saved_stack = SavedExecutionContextStack {
            storage: core::mem::take(&mut *self.execution_context_stack_storage.borrow_mut()),
            length: self.head.execution_context_stack_length.get(),
            running_execution_context: self.head.running_execution_context.replace(core::ptr::null_mut()),
            type_error_realm_override: self.head.type_error_realm_override.take(),
            type_error_realm_override_depth: self.head.type_error_realm_override_depth.replace(0),
        };
        self.saved_execution_context_stacks.borrow_mut().push(saved_stack);
        self.install_execution_context_stack(Vec::new(), 0);
    }

    /// VM::clear_execution_context_stack(): forgets every context on the execution context stack, keeping the stack's
    /// storage.
    pub fn clear_execution_context_stack(&self) {
        self.head.execution_context_stack_length.set(0);
        self.head.running_execution_context.set(core::ptr::null_mut());
        self.head.type_error_realm_override.set(None);
        self.head.type_error_realm_override_depth.set(0);
    }

    /// VM::restore_execution_context_stack(): replaces the execution context stack with the one the matching
    /// save_execution_context_stack() set aside.
    pub fn restore_execution_context_stack(&self) {
        let saved_stack = self
            .saved_execution_context_stacks
            .borrow_mut()
            .pop()
            .expect("a saved execution context stack to restore");
        self.install_execution_context_stack(saved_stack.storage, saved_stack.length);
        self.head
            .running_execution_context
            .set(saved_stack.running_execution_context);
        self.head
            .type_error_realm_override
            .set(saved_stack.type_error_realm_override);
        self.head
            .type_error_realm_override_depth
            .set(saved_stack.type_error_realm_override_depth);
    }

    /// The context below the running one, the second to top element of the execution context stack.
    pub fn previous_execution_context(&self) -> Option<NonNull<ExecutionContext>> {
        let mut previous_execution_context = None;
        let mut found_running_execution_context = false;
        self.for_each_execution_context_top_to_bottom(|execution_context| {
            if !found_running_execution_context {
                found_running_execution_context = true;
                return ControlFlow::Continue(());
            }
            previous_execution_context = Some(NonNull::from(execution_context));
            ControlFlow::Break(())
        });
        previous_execution_context
    }

    pub fn roots(&self) -> &RootSet {
        &self.roots
    }

    fn gather_roots(&self, visitor: &mut Visitor) {
        self.for_each_execution_context_top_to_bottom(|context| {
            context.trace(visitor);
            ControlFlow::Continue(())
        });
        for saved_stack in self.saved_execution_context_stacks.borrow().iter() {
            for_each_execution_context_top_to_bottom_of(
                saved_stack.length,
                |index| {
                    let entry = saved_stack.storage[..saved_stack.length].get(index)?;
                    Some((
                        NonNull::new(entry.execution_context.get()).expect("every entry on the stack has its context"),
                        entry.previous_running_execution_context.get(),
                    ))
                },
                saved_stack.running_execution_context,
                |context| {
                    context.trace(visitor);
                    ControlFlow::Continue(())
                },
            );
            saved_stack.type_error_realm_override.trace(visitor);
        }
        self.roots.trace(visitor);
        self.empty_string.trace(visitor);
        self.single_ascii_character_strings.trace(visitor);
        self.numeric_string_cache.trace(visitor);
        self.large_numeric_string_cache.trace(visitor);
        self.cached_strings.trace(visitor);
        self.well_known_symbols.trace(visitor);
        self.global_symbol_registry.trace(visitor);
        self.head.type_error_realm_override.trace(visitor);
        self.job_queues.trace(visitor);
        self.loaded_modules.trace(visitor);
        self.finalization_registries_with_dead_cells.trace(visitor);
        if let Some(debugger) = self.debugger.borrow().as_ref() {
            debugger.trace(visitor);
        }
    }

    pub fn enable_debugging(&self) {
        if self.debugger.borrow().is_some() {
            return;
        }
        let debugger = Rc::new(Debugger::new());
        self.head.debugger.set(Rc::as_ptr(&debugger).cast_mut().cast());
        *self.debugger.borrow_mut() = Some(debugger);
    }

    pub fn disable_debugging(&self) {
        let debugger = self.debugger.borrow_mut().take();
        if let Some(debugger) = &debugger {
            assert!(!debugger.is_paused());
        }
        self.head.debugger.set(core::ptr::null_mut());
        drop(debugger);
    }

    #[inline]
    pub fn debugging_enabled(&self) -> bool {
        !self.head.debugger.get().is_null()
    }

    #[inline]
    pub fn debugger(&self) -> Option<Rc<Debugger>> {
        if !self.debugging_enabled() {
            return None;
        }
        self.debugger.borrow().clone()
    }

    pub fn register_weak_container(&self, weak_container: WeakContainer) {
        self.weak_containers.borrow_mut().push(weak_container);
    }

    /// Has the weak containers that survived this collection forget the cells that died in it, as LibGC does before
    /// it runs its sweep callbacks.
    fn remove_dead_cells_from_weak_containers(&self) {
        self.weak_containers.borrow_mut().retain(|weak_container| {
            if cell_is_dead(weak_container.owner()) {
                return false;
            }
            weak_container.remove_dead_cells(self);
            true
        });
    }

    /// Hands `finalization_registry` to HostEnqueueFinalizationRegistryCleanupJob once the collection in progress is
    /// over. Only a weak container's remove_dead_cells() calls this.
    pub fn enqueue_finalization_registry_cleanup_job_after_collection(
        &self,
        finalization_registry: Gc<FinalizationRegistry>,
    ) {
        let mut finalization_registries = self.finalization_registries_with_dead_cells.borrow_mut();
        if finalization_registries.is_empty() {
            let context = core::ptr::from_ref::<Vm>(self).cast_mut().cast();
            // SAFETY: The VM outlives its heap, which runs the task before it is destroyed.
            unsafe {
                self.heap()
                    .enqueue_post_gc_task(enqueue_cleanup_jobs_of_finalization_registries_with_dead_cells, context);
            };
        }
        finalization_registries.push(finalization_registry);
    }

    fn enqueue_cleanup_jobs_of_finalization_registries_with_dead_cells(&self) {
        // NB: Each registry stays in the rooted list until it is handed to the host, which may allocate.
        loop {
            let finalization_registry = {
                let mut finalization_registries = self.finalization_registries_with_dead_cells.borrow_mut();
                if finalization_registries.is_empty() {
                    break;
                }
                finalization_registries.remove(0)
            };
            (self.host_enqueue_finalization_registry_cleanup_job.get())(self, finalization_registry);
        }
    }

    /// Forgets the intrinsic accessors and unimplemented properties of objects that died in this collection.
    fn remove_dead_objects_from_intrinsic_accessors_and_unimplemented_properties(&self) {
        let is_live = |object: usize| {
            // SAFETY: The maps only hold objects that were live when their entries were added, and a dead object is
            //         intact until the sweep that follows this callback.
            let object = unsafe { Gc::from_non_null(NonNull::new_unchecked(object as *mut Object)) };
            !cell_is_dead(object)
        };
        self.intrinsic_accessors
            .borrow_mut()
            .retain(|&object, _| is_live(object));
        self.unimplemented_properties
            .borrow_mut()
            .retain(|&object, _| is_live(object));
    }

    pub fn intrinsic_accessors(&self) -> &RefCell<IntrinsicAccessorMap> {
        &self.intrinsic_accessors
    }

    pub fn unimplemented_properties(&self) -> &RefCell<UnimplementedPropertyMap> {
        &self.unimplemented_properties
    }

    pub fn on_unimplemented_property_access(&self) -> Option<OnUnimplementedPropertyAccess> {
        self.on_unimplemented_property_access.get()
    }

    pub fn set_on_unimplemented_property_access(&self, callback: Option<OnUnimplementedPropertyAccess>) {
        self.on_unimplemented_property_access.set(callback);
    }

    pub fn options(&self) -> VmOptions {
        self.options
    }

    pub fn embedder(&self) -> Option<Embedder> {
        self.embedder.get()
    }

    pub fn set_embedder(&self, embedder: Option<Embedder>) {
        self.embedder.set(embedder);
    }

    /// The Agent Record of the surrounding agent.
    pub fn agent(&self) -> AgentRecord {
        self.agent.get()
    }

    pub fn set_agent(&self, agent: AgentRecord) {
        self.agent.set(agent);
    }

    pub fn host_classes(&self) -> &RefCell<HostClassRegistry> {
        &self.host_classes
    }

    /// The realm vm.throw_completion() creates a TypeError in.
    pub(crate) fn type_error_realm(&self) -> Option<Gc<Realm>> {
        if let Some(realm) = self.head.type_error_realm_override.get()
            && self.head.execution_context_stack_length.get() == self.head.type_error_realm_override_depth.get()
        {
            return Some(realm);
        }
        self.current_realm()
    }

    /// Has TypeErrors thrown at the current execution context stack depth created in `realm` until the scope ends.
    /// The override only applies at the depth the scope was created at, so callees pushed while it is alive are
    /// unaffected.
    pub fn type_error_realm_scope(&self, realm: Gc<Realm>) -> TypeErrorRealmScope<'_> {
        TypeErrorRealmScope {
            vm: self,
            previous: self.override_type_error_realm(realm),
        }
    }

    /// What type_error_realm_scope() does, for an embedder that cannot hold the scope: overrides the realm of
    /// TypeErrors at the current execution context stack depth, and returns the override this replaces, which the
    /// caller must put back with restore_type_error_realm_override().
    pub fn override_type_error_realm(&self, realm: Gc<Realm>) -> TypeErrorRealmOverride {
        let previous = TypeErrorRealmOverride {
            realm: self.head.type_error_realm_override.get(),
            depth: self.head.type_error_realm_override_depth.get(),
        };
        self.head.type_error_realm_override.set(Some(realm));
        self.head
            .type_error_realm_override_depth
            .set(self.head.execution_context_stack_length.get());
        previous
    }

    pub fn restore_type_error_realm_override(&self, previous: TypeErrorRealmOverride) {
        self.head.type_error_realm_override.set(previous.realm);
        self.head.type_error_realm_override_depth.set(previous.depth);
    }

    /// The frames of every execution context, from the running one down, with where each is in its executable.
    pub fn stack_trace(&self) -> Vec<StackTraceElement> {
        let mut stack_trace = Vec::new();
        self.for_each_execution_context_top_to_bottom(|context| {
            // NB: Builtins written in JavaScript show up like other builtins, without a position in their source.
            let runs_builtin = context
                .function
                .get()
                .is_some_and(|function| function.is::<NativeJavaScriptBackedFunction>());
            let source_range =
                context.executable.get().filter(|_| !runs_builtin).map(|executable| {
                    Executable::from_head(executable).get_source_range(context.program_counter.get())
                });
            stack_trace.push(StackTraceElement {
                execution_context: NonNull::from(context),
                source_range,
            });
            ControlFlow::Continue(())
        });
        stack_trace
    }

    /// Drops the weak cache entries of strings that died in this collection.
    fn remove_dead_strings_from_weak_caches(&self) {
        let is_dead = |string: Gc<PrimitiveString>| !string.header.mark.get();
        for cache_slot in self.fly_string_cache.iter() {
            if cache_slot.get().is_some_and(is_dead) {
                cache_slot.set(None);
            }
        }
        for entry in self.string_to_atom_cache.borrow_mut().iter_mut() {
            if entry.string.is_some_and(is_dead) {
                *entry = StringToAtomCacheEntry::default();
            }
        }
        self.interned_strings.borrow_mut().retain(|_, string| !is_dead(*string));
    }

    /// Forgets the cells that died in this collection from the inline caches of executables and static call sites.
    fn remove_dead_cells_from_property_lookup_caches(&self) {
        self.executables.borrow_mut().retain(|executable| {
            if cell_is_dead(*executable) {
                return false;
            }
            executable.remove_dead_cells();
            true
        });
        self.static_property_lookup_caches.remove_dead_entries();
        self.keyed_property_lookup_cache.remove_dead_entries();
        self.prototype_transition_cache.remove_dead_entries();
        self.keyed_property_store_cache.remove_dead_entries();
    }

    pub fn register_executable(&self, executable: Gc<Executable>) {
        self.executables.borrow_mut().push(executable);
    }

    pub fn static_property_lookup_cache(&self, site: StaticPropertyLookupCacheSite) -> &PropertyLookupCache {
        self.static_property_lookup_caches.get(site)
    }

    pub fn keyed_property_lookup_cache(&self) -> &KeyedPropertyLookupCache {
        &self.keyed_property_lookup_cache
    }

    pub fn prototype_transition_cache(&self) -> &PrototypeTransitionCache {
        &self.prototype_transition_cache
    }

    pub fn keyed_property_store_cache(&self) -> &KeyedPropertyLookupCache {
        &self.keyed_property_store_cache
    }

    pub fn keyed_property_store_scratch_cache(&self) -> &PropertyLookupCache {
        &self.keyed_property_store_scratch_cache
    }

    /// Stores the value the running frame returns and clears its exception, for the slow paths that leave the
    /// interpreter on a suspension.
    pub fn do_return(&self, value: Value) {
        let value = if value.is_empty() { Value::UNDEFINED } else { value };
        let context = self.running_execution_context_ref();
        context.register(libjs_abi::register::RETURN_VALUE).set(value);
        context.register(libjs_abi::register::EXCEPTION).set(Value::EMPTY);
    }

    /// The executable of the running execution context.
    pub fn current_executable(&self) -> Gc<Executable> {
        let context = self
            .running_execution_context()
            .expect("there is a running execution context");
        // SAFETY: The running execution context is live.
        let executable = unsafe { context.as_ref() }
            .executable
            .get()
            .expect("the running execution context runs an executable");
        // SAFETY: An Executable starts with its head.
        unsafe { Gc::from_non_null(executable.as_non_null().cast()) }
    }

    pub fn host_ensure_can_add_private_element(&self) -> HostEnsureCanAddPrivateElement {
        self.host_ensure_can_add_private_element.get()
    }

    pub fn set_host_ensure_can_add_private_element(&self, hook: HostEnsureCanAddPrivateElement) {
        self.host_ensure_can_add_private_element.set(hook);
    }

    pub fn host_get_code_for_eval(&self) -> HostGetCodeForEval {
        self.host_get_code_for_eval.get()
    }

    pub fn set_host_get_code_for_eval(&self, hook: HostGetCodeForEval) {
        self.host_get_code_for_eval.set(hook);
    }

    pub fn host_ensure_can_compile_strings(&self) -> HostEnsureCanCompileStrings {
        self.host_ensure_can_compile_strings.get()
    }

    pub fn set_host_ensure_can_compile_strings(&self, hook: HostEnsureCanCompileStrings) {
        self.host_ensure_can_compile_strings.set(hook);
    }

    pub fn host_promise_rejection_tracker(&self) -> HostPromiseRejectionTracker {
        self.host_promise_rejection_tracker.get()
    }

    pub fn set_host_promise_rejection_tracker(&self, hook: HostPromiseRejectionTracker) {
        self.host_promise_rejection_tracker.set(hook);
    }

    pub fn host_call_job_callback(&self) -> HostCallJobCallback {
        self.host_call_job_callback.get()
    }

    pub fn set_host_call_job_callback(&self, hook: HostCallJobCallback) {
        self.host_call_job_callback.set(hook);
    }

    pub fn host_enqueue_finalization_registry_cleanup_job(&self) -> HostEnqueueFinalizationRegistryCleanupJob {
        self.host_enqueue_finalization_registry_cleanup_job.get()
    }

    pub fn set_host_enqueue_finalization_registry_cleanup_job(&self, hook: HostEnqueueFinalizationRegistryCleanupJob) {
        self.host_enqueue_finalization_registry_cleanup_job.set(hook);
    }

    pub fn host_enqueue_promise_job(&self) -> HostEnqueuePromiseJob {
        self.host_enqueue_promise_job.get()
    }

    pub fn set_host_enqueue_promise_job(&self, hook: HostEnqueuePromiseJob) {
        self.host_enqueue_promise_job.set(hook);
    }

    pub fn host_make_job_callback(&self) -> HostMakeJobCallback {
        self.host_make_job_callback.get()
    }

    pub fn set_host_make_job_callback(&self, hook: HostMakeJobCallback) {
        self.host_make_job_callback.set(hook);
    }

    pub fn host_promise_job_queue_is_empty(&self) -> HostPromiseJobQueueIsEmpty {
        self.host_promise_job_queue_is_empty.get()
    }

    pub fn set_host_promise_job_queue_is_empty(&self, hook: HostPromiseJobQueueIsEmpty) {
        self.host_promise_job_queue_is_empty.set(hook);
    }

    pub fn host_system_utc_epoch_nanoseconds(&self) -> HostSystemUTCEpochNanoseconds {
        self.host_system_utc_epoch_nanoseconds.get()
    }

    pub fn set_host_system_utc_epoch_nanoseconds(&self, hook: HostSystemUTCEpochNanoseconds) {
        self.host_system_utc_epoch_nanoseconds.set(hook);
    }

    pub fn host_unrecognized_date_string(&self) -> HostUnrecognizedDateString {
        self.host_unrecognized_date_string.get()
    }

    pub fn set_host_unrecognized_date_string(&self, hook: HostUnrecognizedDateString) {
        self.host_unrecognized_date_string.set(hook);
    }

    pub fn set_on_promise_unhandled_rejection(&self, callback: Option<PromiseRejectionCallback>) {
        self.on_promise_unhandled_rejection.set(callback);
    }

    pub fn set_on_promise_rejection_handled(&self, callback: Option<PromiseRejectionCallback>) {
        self.on_promise_rejection_handled.set(callback);
    }

    fn job_queues(&self) -> Gc<JobQueues> {
        *self.job_queues.get().expect("the VM allocates its job queues")
    }

    pub fn run_executable_depth(&self) -> &Cell<u32> {
        &self.run_executable_depth
    }

    pub fn run_queued_promise_jobs(&self) {
        if self.job_queues().promise_jobs.borrow().is_empty() {
            return;
        }
        self.run_queued_promise_jobs_impl();
    }

    fn run_queued_promise_jobs_impl(&self) {
        let job_queues = self.job_queues();
        loop {
            let Some(job) = job_queues.promise_jobs.borrow_mut().pop_front() else {
                break;
            };
            let _ = HeapFunction::call(job, self);
        }
    }

    // 9.5.4 HostEnqueuePromiseJob ( job, realm ), https://tc39.es/ecma262/#sec-hostenqueuepromisejob
    pub fn enqueue_promise_job(&self, job: Gc<HeapFunction>, _realm: Option<Gc<Realm>>) {
        // An implementation of HostEnqueuePromiseJob must conform to the requirements in 9.5 as well as the following:
        // - FIXME: If realm is not null, each time job is invoked the implementation must perform implementation-defined steps such that execution is prepared to evaluate ECMAScript code at the time of job's invocation.
        // - FIXME: Let scriptOrModule be GetActiveScriptOrModule() at the time HostEnqueuePromiseJob is invoked. If realm is not null, each time job is invoked the implementation must perform implementation-defined steps
        //          such that scriptOrModule is the active script or module at the time of job's invocation.
        // - Jobs must run in the same order as the HostEnqueuePromiseJob invocations that scheduled them.
        self.job_queues().promise_jobs.borrow_mut().push_back(job);
    }

    pub fn run_queued_finalization_registry_cleanup_jobs(&self) {
        let job_queues = self.job_queues();
        loop {
            let Some(finalization_registry) = job_queues.finalization_registry_cleanup_jobs.borrow_mut().pop() else {
                break;
            };
            // FIXME: Handle any uncatched exceptions here.
            let result = finalization_registry.cleanup(self, None);
            if result.is_err() && finalization_registry.has_empty_cells() {
                job_queues
                    .finalization_registry_cleanup_jobs
                    .borrow_mut()
                    .push(finalization_registry);
            }
        }
    }

    // 9.10.4.1 HostEnqueueFinalizationRegistryCleanupJob ( finalizationRegistry ), https://tc39.es/ecma262/#sec-host-cleanup-finalization-registry
    pub fn enqueue_finalization_registry_cleanup_job(&self, finalization_registry: Gc<FinalizationRegistry>) {
        self.job_queues()
            .finalization_registry_cleanup_jobs
            .borrow_mut()
            .push(finalization_registry);
    }

    // 27.2.1.9 HostPromiseRejectionTracker ( promise, operation ), https://tc39.es/ecma262/#sec-host-promise-rejection-tracker
    pub fn promise_rejection_tracker(&self, promise: Gc<Promise>, operation: RejectionOperation) {
        match operation {
            RejectionOperation::Reject => {
                // A promise was rejected without any handlers
                if let Some(on_promise_unhandled_rejection) = self.on_promise_unhandled_rejection.get() {
                    on_promise_unhandled_rejection(self, promise);
                }
            }
            RejectionOperation::Handle => {
                // A handler was added to an already rejected promise
                if let Some(on_promise_rejection_handled) = self.on_promise_rejection_handled.get() {
                    on_promise_rejection_handled(self, promise);
                }
            }
        }
    }

    pub fn host_resize_array_buffer(&self) -> HostResizeArrayBuffer {
        self.host_resize_array_buffer.get()
    }

    pub fn set_host_resize_array_buffer(&self, hook: HostResizeArrayBuffer) {
        self.host_resize_array_buffer.set(hook);
    }

    pub fn host_grow_shared_array_buffer(&self) -> HostGrowSharedArrayBuffer {
        self.host_grow_shared_array_buffer.get()
    }

    pub fn set_host_grow_shared_array_buffer(&self, hook: HostGrowSharedArrayBuffer) {
        self.host_grow_shared_array_buffer.set(hook);
    }

    pub fn host_load_imported_module(&self) -> HostLoadImportedModule {
        self.host_load_imported_module.get()
    }

    pub fn set_host_load_imported_module(&self, hook: HostLoadImportedModule) {
        self.host_load_imported_module.set(hook);
    }

    pub fn host_get_import_meta_properties(&self) -> HostGetImportMetaProperties {
        self.host_get_import_meta_properties.get()
    }

    pub fn set_host_get_import_meta_properties(&self, hook: HostGetImportMetaProperties) {
        self.host_get_import_meta_properties.set(hook);
    }

    pub fn host_finalize_import_meta(&self) -> HostFinalizeImportMeta {
        self.host_finalize_import_meta.get()
    }

    pub fn set_host_finalize_import_meta(&self, hook: HostFinalizeImportMeta) {
        self.host_finalize_import_meta.set(hook);
    }

    pub fn host_get_supported_import_attributes(&self) -> HostGetSupportedImportAttributes {
        self.host_get_supported_import_attributes.get()
    }

    pub fn set_host_get_supported_import_attributes(&self, hook: HostGetSupportedImportAttributes) {
        self.host_get_supported_import_attributes.set(hook);
    }

    pub fn set_dynamic_imports_allowed(&self, value: bool) {
        self.dynamic_imports_allowed.set(value);
    }

    pub fn enter_module_execution(&self) {
        self.module_execution_depth.set(self.module_execution_depth.get() + 1);
    }

    pub fn leave_module_execution(&self) {
        assert!(self.module_execution_depth.get() > 0);
        self.module_execution_depth.set(self.module_execution_depth.get() - 1);
    }

    pub fn is_executing_module(&self) -> bool {
        self.module_execution_depth.get() > 0
    }

    pub fn increment_module_async_evaluation_count(&self) -> u64 {
        let count = self.module_async_evaluation_count.get();
        self.module_async_evaluation_count.set(count + 1);
        count
    }

    fn loaded_modules(&self) -> Gc<LoadedModules> {
        *self
            .loaded_modules
            .get()
            .expect("the VM allocates its list of loaded modules")
    }

    pub fn next_private_environment_id(&self) -> &Cell<u64> {
        &self.next_private_environment_id
    }

    pub fn string_to_atom_cache(&self) -> &RefCell<Box<[StringToAtomCacheEntry; STRING_TO_ATOM_CACHE_SIZE]>> {
        &self.string_to_atom_cache
    }

    pub fn fly_string_cache(&self) -> &[Cell<Option<Gc<PrimitiveString>>>; FLY_STRING_CACHE_SIZE] {
        &self.fly_string_cache
    }

    pub fn interned_strings(&self) -> &RefCell<HashMap<usize, Gc<PrimitiveString>, foldhash::fast::RandomState>> {
        &self.interned_strings
    }

    pub fn numeric_string_cache(&self) -> &[Cell<Option<Gc<PrimitiveString>>>; NUMERIC_STRING_CACHE_SIZE] {
        &self.numeric_string_cache
    }

    pub fn large_numeric_string_cache(&self) -> &[NumericStringCacheEntry; LARGE_NUMERIC_STRING_CACHE_SIZE] {
        &self.large_numeric_string_cache
    }

    pub fn empty_string(&self) -> Gc<PrimitiveString> {
        *self.empty_string.get().expect("the VM allocates the empty string")
    }

    pub fn single_ascii_character_string(&self, character: u8) -> Gc<PrimitiveString> {
        assert!(character < 0x80);
        self.single_ascii_character_strings
            .get()
            .expect("the VM allocates the single ASCII character strings")[usize::from(character)]
    }

    pub fn cached_strings(&self) -> &CachedStrings {
        self.cached_strings.get().expect("the VM allocates the cached strings")
    }

    pub fn well_known_symbols(&self) -> &WellKnownSymbols {
        self.well_known_symbols
            .get()
            .expect("the VM creates the well-known symbols")
    }

    pub fn global_symbol_registry(&self) -> Gc<GlobalSymbolRegistry> {
        *self
            .global_symbol_registry
            .get()
            .expect("the VM creates the global symbol registry")
    }

    pub fn register_native_function(&self, entry: NativeFunctionTableEntry) -> u32 {
        let function = entry.function.expect("a native function has a function pointer");
        let key = (function as usize, entry.function_type as u32);
        if let Some(index) = self.native_function_indices.borrow().get(&key) {
            return *index;
        }

        let index = self.native_function_count.get();
        assert!(
            (index as usize) < NATIVE_FUNCTION_TABLE_CAPACITY,
            "the native function table is full"
        );
        self.native_function_table.borrow_mut()[index as usize] = entry;
        self.native_function_count.set(index + 1);
        self.native_function_indices.borrow_mut().insert(key, index);
        index
    }

    pub fn native_function(&self, index: u32, expected_type: NativeFunctionType) -> RawNativeFunctionPointer {
        let table = self.native_function_table.borrow();
        let entry = &table[(index & NATIVE_FUNCTION_TABLE_INDEX_MASK) as usize];
        assert!(entry.function_type == expected_type);
        assert!(entry.function.is_some());
        entry.function
    }

    /// Pushes `context`, unless so little of the native stack is left that the next call could overflow it.
    pub fn push_execution_context_checking_stack_space(
        &self,
        context: NonNull<ExecutionContext>,
    ) -> ThrowCompletionOr<()> {
        // Ensure we got some stack space left, so the next function call doesn't kill us.
        if self.did_reach_stack_space_limit() {
            return self.throw_completion(ErrorKind::InternalError, ErrorType::CallStackSizeExceeded, &[]);
        }
        self.push_execution_context(context);
        Ok(())
    }

    // 9.4.1 GetActiveScriptOrModule ( ), https://tc39.es/ecma262/#sec-getactivescriptormodule
    pub fn get_active_script_or_module(&self) -> ScriptOrModule {
        // 1. If the execution context stack is empty, return null.
        if self.running_execution_context().is_none() {
            return ScriptOrModule::Empty;
        }

        // 2. Let ec be the topmost execution context on the execution context stack whose ScriptOrModule component is not null.
        let mut script_or_module = ScriptOrModule::Empty;
        self.for_each_execution_context_top_to_bottom(|execution_context| {
            script_or_module = execution_context.script_or_module.get();
            if matches!(script_or_module, ScriptOrModule::Empty) {
                return ControlFlow::Continue(());
            }
            ControlFlow::Break(())
        });

        // 3. If no such execution context exists, return null. Otherwise, return ec's ScriptOrModule.
        script_or_module
    }

    pub(crate) fn running_execution_context_ref(&self) -> &ExecutionContext {
        let context = self
            .running_execution_context()
            .expect("there is a running execution context");
        // SAFETY: The running execution context is live while it runs, which outlasts any use of this reference by
        // the code running in it.
        unsafe { context.as_ref() }
    }

    /// The number of arguments the running execution context has slots for.
    pub fn argument_count(&self) -> usize {
        self.running_execution_context_ref().argument_count.get() as usize
    }

    pub fn argument(&self, index: usize) -> Value {
        self.running_execution_context_ref().argument(index)
    }

    pub fn this_value(&self) -> Value {
        let this_value = self.running_execution_context_ref().this_value.get();
        assert!(!this_value.is_empty(), "the running execution context has a this value");
        this_value
    }

    pub fn lexical_environment(&self) -> Option<Gc<Environment>> {
        self.running_execution_context_ref().lexical_environment.get()
    }

    pub fn active_function_object(&self) -> Option<Gc<FunctionObject>> {
        self.running_execution_context_ref().function.get()
    }

    pub fn active_shared_function_data(&self) -> Option<Gc<SharedFunctionInstanceData>> {
        let function = self.active_function_object()?;
        if let Some(ecmascript_function) = as_ecmascript_function_object(function) {
            return Some(ecmascript_function.shared_data());
        }
        if let Some(native_javascript_backed_function) = function.downcast::<NativeJavaScriptBackedFunction>() {
            return Some(native_javascript_backed_function.shared_data());
        }
        None
    }
}

impl Vm {
    pub fn execution_context_stack_is_empty(&self) -> bool {
        self.head.execution_context_stack_length.get() == 0
    }

    pub fn variable_environment(&self) -> Option<Gc<Environment>> {
        self.running_execution_context_ref().variable_environment.get()
    }

    /// The Realm of the running execution context, which C++ VM::realm() dereferences.
    fn running_realm(&self) -> Gc<Realm> {
        self.current_realm().expect("the running execution context has a realm")
    }

    pub fn global_object(&self) -> Gc<Object> {
        self.running_realm().global_object()
    }

    pub fn global_declarative_environment(&self) -> Gc<DeclarativeEnvironment> {
        self.running_realm().global_declarative_environment()
    }

    // 9.1.2.1 GetIdentifierReference ( env, name, strict ), https://tc39.es/ecma262/#sec-getidentifierreference
    pub fn get_identifier_reference(
        &self,
        environment: Option<Gc<Environment>>,
        name: Utf16FlyString,
        strict: Strict,
        hops: usize,
    ) -> ThrowCompletionOr<Reference> {
        // 1. If env is the value null, then
        let Some(environment) = environment else {
            // a. Return the Reference Record { [[Base]]: unresolvable, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
            return Ok(Reference::with_base_type(
                BaseType::Unresolvable,
                PropertyKey::from(name),
                strict,
            ));
        };

        // 2. Let exists be ? env.HasBinding(name).
        let mut index = None;
        let exists = environment.has_binding(self, &name, Some(&mut index))?;

        // Note: This is an optimization for looking up the same reference.
        let environment_coordinate = index.map(|index| EnvironmentCoordinate {
            hops: u32::try_from(hops).expect("the hop count fits in u32"),
            index: u32::try_from(index).expect("the binding index fits in u32"),
        });

        // 3. If exists is true, then
        if exists {
            // a. Return the Reference Record { [[Base]]: env, [[ReferencedName]]: name, [[Strict]]: strict, [[ThisValue]]: empty }.
            return Ok(Reference::with_base_environment(
                environment,
                name,
                strict,
                environment_coordinate,
            ));
        }
        // 4. Else,
        // a. Let outer be env.[[OuterEnv]].
        // b. Return ? GetIdentifierReference(outer, name, strict).
        self.get_identifier_reference(environment.outer_environment(), name, strict, hops + 1)
    }

    // 9.4.2 ResolveBinding ( name [ , env ] ), https://tc39.es/ecma262/#sec-resolvebinding
    pub fn resolve_binding(
        &self,
        name: &Utf16FlyString,
        strict: Strict,
        environment: Option<Gc<Environment>>,
    ) -> ThrowCompletionOr<Reference> {
        // 1. If env is not present or if env is undefined, then
        //     a. Set env to the running execution context's LexicalEnvironment.
        let environment = environment.or_else(|| self.lexical_environment());

        // 2. Assert: env is an Environment Record.
        let environment = environment.expect("ResolveBinding has an environment to resolve in");

        // 3. If the source text matched by the syntactic production that is being evaluated is contained in strict mode code, let strict be true; else let strict be false.
        // NOTE: We take this as a parameter.

        // 4. Return ? GetIdentifierReference(env, name, strict).
        self.get_identifier_reference(Some(environment), name.clone(), strict, 0)

        // NOTE: The spec says:
        //       Note: The result of ResolveBinding is always a Reference Record whose [[ReferencedName]] field is name.
        //       But this is not actually correct as GetIdentifierReference (or really the methods it calls) can throw.
    }

    // 9.4.4 ResolveThisBinding ( ), https://tc39.es/ecma262/#sec-resolvethisbinding
    pub fn resolve_this_binding(&self) -> ThrowCompletionOr<Value> {
        // 1. Let envRec be GetThisEnvironment().
        let environment = get_this_environment(self);

        // 2. Return ? envRec.GetThisBinding().
        environment.get_this_binding(self)
    }

    // 9.4.5 GetNewTarget ( ), https://tc39.es/ecma262/#sec-getnewtarget
    pub fn get_new_target(&self) -> Value {
        // 1. Let envRec be GetThisEnvironment().
        let environment = get_this_environment(self);

        // 2. Assert: envRec has a [[NewTarget]] field.
        // 3. Return envRec.[[NewTarget]].
        environment
            .downcast::<FunctionEnvironment>()
            .expect("the this environment of new.target is a function environment")
            .new_target()
    }

    // 9.4.5 GetGlobalObject ( ), https://tc39.es/ecma262/#sec-getglobalobject
    pub fn get_global_object(&self) -> Gc<Object> {
        // 1. Let currentRealm be the current Realm Record.
        let current_realm = self.current_realm().expect("there is a current realm");

        // 2. Return currentRealm.[[GlobalObject]].
        current_realm.global_object()
    }
}

impl Drop for Vm {
    fn drop(&mut self) {
        // The debugger holds weak references to executables, which go away with the heap.
        drop(self.debugger.take());
        // The heap's final collection destroys every cell, so it has to happen while the rest of the VM exists.
        drop(self.heap.take());
    }
}

unsafe extern "C" fn gather_roots(context: *mut c_void, visitor: *mut GCVisitor) {
    // SAFETY: The heap was created with the VM as its context, and LibGC passes a live visitor.
    let (vm, mut visitor) = unsafe { (&*context.cast::<Vm>(), Visitor::from_raw(visitor)) };
    vm.gather_roots(&mut visitor);
}

unsafe extern "C" fn sweep(context: *mut c_void) {
    // SAFETY: The sweep callback was registered with the VM as its context.
    let vm = unsafe { &*context.cast::<Vm>() };
    vm.remove_dead_cells_from_weak_containers();
    vm.remove_dead_strings_from_weak_caches();
    vm.remove_dead_cells_from_property_lookup_caches();
    vm.remove_dead_objects_from_intrinsic_accessors_and_unimplemented_properties();
    if let Some(debugger) = vm.debugger() {
        debugger.remove_dead_executables();
    }
}

unsafe extern "C" fn enqueue_cleanup_jobs_of_finalization_registries_with_dead_cells(context: *mut c_void) {
    // SAFETY: The task was enqueued with the VM as its context.
    let vm = unsafe { &*context.cast::<Vm>() };
    vm.enqueue_cleanup_jobs_of_finalization_registries_with_dead_cells();
}

/// VM::TypeErrorRealmScope.
pub struct TypeErrorRealmScope<'vm> {
    vm: &'vm Vm,
    previous: TypeErrorRealmOverride,
}

impl Drop for TypeErrorRealmScope<'_> {
    fn drop(&mut self) {
        self.vm.restore_type_error_realm_override(self.previous);
    }
}

/// The realm that TypeErrors are created in at an execution context stack depth, if any.
#[derive(Clone, Copy)]
pub struct TypeErrorRealmOverride {
    pub realm: Option<Gc<Realm>>,
    pub depth: usize,
}

/// An element of VM::stack_trace(): an execution context and where it is in its executable, if it runs one.
pub struct StackTraceElement {
    pub execution_context: NonNull<ExecutionContext>,
    pub source_range: Option<SourceRange>,
}

/// TextCodec's UTF-8 decoder, unless the bytes start with a byte order mark, which picks the encoding instead. Both
/// replace what they cannot decode.
fn decode_module_source(bytes: &[u8]) -> Vec<u16> {
    let decode_utf16 = |bytes: &[u8], code_unit_from_bytes: fn([u8; 2]) -> u16| {
        let (chunks, remainder) = bytes.as_chunks::<2>();
        let mut code_units: Vec<u16> = char::decode_utf16(chunks.iter().map(|&chunk| code_unit_from_bytes(chunk)))
            .map(|decoded| decoded.unwrap_or(char::REPLACEMENT_CHARACTER))
            .collect::<String>()
            .encode_utf16()
            .collect();
        if !remainder.is_empty() {
            code_units.push(char::REPLACEMENT_CHARACTER as u16);
        }
        code_units
    };
    if let Some(bytes) = bytes.strip_prefix(b"\xEF\xBB\xBF") {
        return String::from_utf8_lossy(bytes).encode_utf16().collect();
    }
    if let Some(bytes) = bytes.strip_prefix(b"\xFE\xFF") {
        return decode_utf16(bytes, u16::from_be_bytes);
    }
    if let Some(bytes) = bytes.strip_prefix(b"\xFF\xFE") {
        return decode_utf16(bytes, u16::from_le_bytes);
    }
    String::from_utf8_lossy(bytes).encode_utf16().collect()
}

/// Whether a file or directory is at `path`, FileSystem::exists().
fn file_exists(path: &str) -> bool {
    std::fs::metadata(path).is_ok()
}

/// FileSystem::is_directory()
fn is_directory(path: &str) -> bool {
    std::fs::metadata(path).is_ok_and(|metadata| metadata.is_dir())
}

fn resolve_module_filename(filename: &str, module_type: &str) -> String {
    let extensions: &[&str] = if module_type == "json" {
        &["json"]
    } else {
        &["js", "mjs"]
    };
    if !file_exists(filename) {
        for extension in extensions {
            // import "./foo" -> import "./foo.ext"
            let resolved_filepath = format!("{filename}.{extension}");
            if file_exists(&resolved_filepath) {
                return resolved_filepath;
            }
        }
    } else if is_directory(filename) {
        for extension in extensions {
            // import "./foo" -> import "./foo/index.ext"
            let resolved_filepath = lexical_path::join(filename, &format!("index.{extension}"));
            if file_exists(&resolved_filepath) {
                return resolved_filepath;
            }
        }
    }
    filename.to_string()
}

impl Vm {
    // 13.3.12.1 Runtime Semantics: Evaluation, https://tc39.es/ecma262/#sec-meta-properties-runtime-semantics-evaluation
    // ImportMeta branch only
    pub fn get_import_meta(&self) -> Gc<Object> {
        // 1. Let module be GetActiveScriptOrModule().
        let ScriptOrModule::Module(module) = self.get_active_script_or_module() else {
            unreachable!("import.meta is only evaluated in the code of a module");
        };

        // 2. Assert: module is a Source Text Module Record.
        let module = module
            .downcast::<SourceTextModule>()
            .expect("import.meta is only evaluated in the code of a Source Text Module Record");

        // 3. Let importMeta be module.[[ImportMeta]].
        // 5. Else,
        if let Some(import_meta) = module.import_meta() {
            // a. Assert: Type(importMeta) is Object.
            // Note: This is always true by the type.

            // b. Return importMeta.
            return import_meta;
        }

        // 4. If importMeta is empty, then
        // a. Set importMeta to OrdinaryObjectCreate(null).
        let realm = self.current_realm().expect("import.meta is evaluated in a realm");
        let import_meta = Object::create(self, realm, None);

        // b. Let importMetaValues be HostGetImportMetaProperties(module).
        let import_meta_values = (self.host_get_import_meta_properties.get())(self, module);

        // c. For each Record { [[Key]], [[Value]] } p of importMetaValues, do
        for index in 0..import_meta_values.len() {
            let (key, value) = import_meta_values.get(index).expect("the index is in bounds");
            // i. Perform ! CreateDataPropertyOrThrow(importMeta, p.[[Key]], p.[[Value]]).
            import_meta.create_data_property_or_throw(self, &key, value).must();
        }

        // d. Perform HostFinalizeImportMeta(importMeta, module).
        (self.host_finalize_import_meta.get())(self, import_meta, module);

        // e. Set module.[[ImportMeta]] to importMeta.
        module.set_import_meta(import_meta);

        // f. Return importMeta.
        import_meta
    }

    fn get_stored_module(&self, filename: &str) -> Option<Gc<Module>> {
        // Note the spec says:
        // If this operation is called multiple times with the same (referrer, specifier) pair and it performs
        // FinishLoadingImportedModule(referrer, specifier, payload, result) where result is a normal completion,
        // then it must perform FinishLoadingImportedModule(referrer, specifier, payload, result) with the same result each time.

        // Editor's Note from https://tc39.es/ecma262/#sec-hostresolveimportedmodule
        // The above text requires that hosts support JSON modules when imported with type: "json" (and HostLoadImportedModule
        // completes normally), but it does not prohibit hosts from supporting JSON modules when imported without type: "json".

        // FIXME: This should probably check referrer as well.
        self.loaded_modules()
            .stored_modules
            .borrow()
            .iter()
            .find(|stored_module| stored_module.filename == filename)
            .map(|stored_module| stored_module.module)
    }

    fn store_module(&self, referrer: ImportedModuleReferrer, filename: String, module: Gc<Module>) {
        self.loaded_modules().stored_modules.borrow_mut().push(StoredModule {
            referrer,
            filename,
            module_type: String::new(),
            module,
            has_once_started_linking: true,
        });
    }

    pub fn link_and_eval_module(&self, module: Gc<CyclicModule>) -> ThrowCompletionOr<()> {
        let filename = module.filename().to_string();
        if !filename.is_empty() {
            let absolute_filename = resolve_module_filename(&lexical_path::absolute_path(".", &filename), "");
            if self.get_stored_module(&absolute_filename).is_none() {
                // Register the entry module before loading dependencies so self-imports resolve to this Module Record.
                self.store_module(
                    ImportedModuleReferrer::CyclicModule(module),
                    absolute_filename,
                    module.upcast(),
                );
            }
        }

        let module: Gc<Module> = module.upcast();
        let promise_capability = module.load_requested_modules(self, ForeignCellSlot::empty());

        let promise = promise_of(promise_capability);
        if promise.state() == PromiseState::Rejected {
            crate::embedding::completion::log_exception_if_enabled(self, promise.result());
            return Err(Throw::new(promise.result()));
        }

        module.link(self)?;

        let evaluated_value = promise_of(module.evaluate(self)?);

        self.run_queued_promise_jobs();
        assert!(self.job_queues().promise_jobs.borrow().is_empty());

        // FIXME: This will break if we start doing promises actually asynchronously.
        assert!(evaluated_value.state() != PromiseState::Pending);

        if evaluated_value.state() == PromiseState::Rejected {
            crate::embedding::completion::log_exception_if_enabled(self, evaluated_value.result());
            return Err(Throw::new(evaluated_value.result()));
        }

        Ok(())
    }

    // 16.2.1.8 HostLoadImportedModule ( referrer, specifier, hostDefined, payload ), https://tc39.es/ecma262/#sec-HostLoadImportedModule
    pub fn load_imported_module(
        vm: &Vm,
        referrer: ImportedModuleReferrer,
        module_request: &ModuleRequest,
        _host_defined: Option<NonNull<c_void>>,
        payload: ImportedModulePayload,
    ) {
        // An implementation of HostLoadImportedModule must conform to the following requirements:
        //
        // - The host environment must perform FinishLoadingImportedModule(referrer, specifier, payload, result),
        //   where result is either a normal completion containing the loaded Module Record or a throw completion,
        //   either synchronously or asynchronously.
        // - If this operation is called multiple times with the same (referrer, specifier) pair and it performs
        //   FinishLoadingImportedModule(referrer, specifier, payload, result) where result is a normal completion,
        //   then it must perform FinishLoadingImportedModule(referrer, specifier, payload, result) with the same result each time.
        // - If moduleRequest.[[Attributes]] has an entry entry such that entry.[[Key]] is "type" and entry.[[Value]] is "json",
        //   when the host environment performs FinishLoadingImportedModule(referrer, moduleRequest, payload, result), result
        //   must either be the Completion Record returned by an invocation of ParseJSONModule or a throw completion.
        // - The operation must treat payload as an opaque value to be passed through to FinishLoadingImportedModule.
        //
        // The actual process performed is host-defined, but typically consists of performing whatever I/O operations are necessary to
        // load the appropriate Module Record. Multiple different (referrer, specifier) pairs may map to the same Module Record instance.
        // The actual mapping semantics is host-defined but typically a normalization process is applied to specifier as part of the
        // mapping process. A typical normalization process would include actions such as expansion of relative and abbreviated path specifiers.

        let module_specifier = Utf16View::of_fly_string(&module_request.module_specifier).to_utf8();

        // Here we check, against the spec, if payload is a promise capability, meaning that this was called for a dynamic import
        if matches!(payload, ImportedModulePayload::PromiseCapability(_)) && !vm.dynamic_imports_allowed.get() {
            // If you are here because you want to enable dynamic module importing make sure it won't be a security problem
            // by checking the default implementation of HostImportModuleDynamically and creating your own hook or calling
            // vm.allow_dynamic_imports().
            finish_loading_imported_module(
                vm,
                referrer,
                module_request,
                payload,
                vm.throw_completion(ErrorKind::InternalError, ErrorType::DynamicImportNotAllowed, &[]),
            );
            return;
        }

        let module_type = module_request
            .attributes
            .iter()
            .find(|attribute| Utf16View::of_string(&attribute.key).to_utf8() == "type")
            .map(|attribute| Utf16View::of_string(&attribute.value).to_utf8())
            .unwrap_or_default();

        let base_filename = match referrer {
            ImportedModuleReferrer::Realm(_) => {
                // Generally within ECMA262 we always get a referencing_script_or_module. However, ShadowRealm gives an explicit null.
                // To get around this is we attempt to get the active script_or_module otherwise we might start loading "random" files from the working directory.
                match vm.get_active_script_or_module() {
                    ScriptOrModule::Empty => ".".to_string(),
                    ScriptOrModule::Script(script) => script.filename().to_string(),
                    ScriptOrModule::Module(module) => module.filename().to_string(),
                }
            }
            ImportedModuleReferrer::Script(script) => script.filename().to_string(),
            ImportedModuleReferrer::CyclicModule(module) => module.filename().to_string(),
        };

        let filename = lexical_path::absolute_path(&lexical_path::dirname(&base_filename), &module_specifier);
        let filename = resolve_module_filename(&filename, &module_type);

        if let Some(loaded_module) = vm.get_stored_module(&filename) {
            finish_loading_imported_module(vm, referrer, module_request, payload, Ok(loaded_module));
            return;
        }

        let module_not_found = || {
            vm.throw_completion(
                ErrorKind::SyntaxError,
                ErrorType::ModuleNotFound,
                &[&module_request.module_specifier],
            )
        };

        let Ok(mut file) = std::fs::File::open(&filename) else {
            finish_loading_imported_module(vm, referrer, module_request, payload, module_not_found());
            return;
        };

        // FIXME: Don't read the file in one go.
        let mut file_content = Vec::new();
        if let Err(error) = std::io::Read::read_to_end(&mut file, &mut file_content) {
            let result = if error.kind() == std::io::ErrorKind::OutOfMemory {
                vm.throw_completion(ErrorKind::InternalError, ErrorType::OutOfMemory, &[])
            } else {
                module_not_found()
            };
            finish_loading_imported_module(vm, referrer, module_request, payload, result);
            return;
        }

        let source = decode_module_source(&file_content);
        let realm = vm.current_realm().expect("modules are loaded in a realm");

        // If moduleRequest.[[Attributes]] has an entry entry such that entry.[[Key]] is "type" and entry.[[Value]] is "json",
        // when the host environment performs FinishLoadingImportedModule(referrer, moduleRequest, payload, result), result
        // must either be the Completion Record returned by an invocation of ParseJSONModule or a throw completion.
        let module: ThrowCompletionOr<Gc<Module>> = if module_type == "json" {
            parse_json_module(vm, realm, Utf16View::Utf16(&source), filename).map(Gc::upcast)
        } else {
            // Note: We treat all files as module, so if a script does not have exports it just runs it.
            let source_code = SourceCode::create(Utf16String::from_utf8(&filename), Utf16String::from_utf16(&source));
            match SourceTextModule::parse(vm, source_code, realm, &filename) {
                Err(errors) => vm.throw_completion_with_message(ErrorKind::SyntaxError, errors[0].to_string()),
                Ok(module) => Ok(module.upcast()),
            }
        };

        if let Ok(module) = module {
            vm.store_module(referrer, module.filename().to_string(), module);
        }

        finish_loading_imported_module(vm, referrer, module_request, payload, module);
    }
}
