/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What IndexedDB asks of LibWeb's C++ code, defined in Libraries/LibWeb/IndexedDB/RustIntegration.cpp.

use std::rc::Rc;

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, Gc, Object, RawNativeFunctionResult, Realm, Trace, Value, Vm};
use liburl_rust::FfiOrigin;
use liburl_rust::origin::Origin;

use crate::bindings::conversions::CppInterface;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::idb_record::StorageSerializationRecord;
use crate::indexed_db::internal::database::Database;
use crate::indexed_db::internal::function::{Function, FunctionCell, call_function};
use crate::indexed_db::internal::key::{Key, KeyValue};
use crate::indexed_db::internal::mutation_log::{TransactionChange, TransactionChanges};
use crate::indexed_db::internal::storage::StorageKey;

unsafe extern "C" {
    fn web_indexed_db_create_dom_exception(name: ak::Span<'_, u8>, message: usize) -> *mut c_void;
    fn web_indexed_db_dom_exception_from_value(value: u64) -> *mut c_void;
    fn web_indexed_db_wrap_dom_exception(realm: *mut c_void, exception: *mut c_void) -> *mut c_void;
    fn web_indexed_db_obtain_a_storage_key(
        realm: *mut c_void,
        ctx: *mut c_void,
        on_origin: unsafe extern "C" fn(*mut c_void, *const FfiOrigin),
    ) -> bool;
    fn web_indexed_db_create_dom_string_list(realm: *mut c_void, strings: *const usize, count: usize) -> *mut c_void;
    fn web_indexed_db_structured_serialize_for_storage(
        realm: *mut c_void,
        value: u64,
        bytes: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    ) -> RawNativeFunctionResult;
    fn web_indexed_db_structured_deserialize(realm: *mut c_void, bytes: ak::Span<'_, u8>) -> RawNativeFunctionResult;
    fn web_indexed_db_queue_a_database_task(cell: *mut c_void, steps: unsafe extern "C" fn(*mut c_void, *mut c_void));
    fn web_indexed_db_run_in_parallel(cell: *mut c_void, steps: unsafe extern "C" fn(*mut c_void, *mut c_void));
    fn web_indexed_db_main_thread_event_loop() -> *mut c_void;
    fn web_indexed_db_enqueue_check_pending_connection_wait(database: *mut c_void);
    fn web_indexed_db_devtools_client_is_active(realm: *mut c_void) -> bool;
    fn web_indexed_db_create_transaction_changes() -> *mut c_void;
    fn web_indexed_db_append_transaction_change(
        changes: *mut c_void,
        kind: u8,
        database_name: usize,
        object_store_name: usize,
        key: *mut c_void,
    );
    fn web_indexed_db_notify_devtools(realm: *mut c_void, changes: *mut c_void);
    fn web_indexed_db_json_number(value: f64) -> *mut c_void;
    fn web_indexed_db_json_string(value: usize) -> *mut c_void;
    fn web_indexed_db_json_binary(bytes: ak::Span<'_, u8>) -> *mut c_void;
    fn web_indexed_db_json_null() -> *mut c_void;
    fn web_indexed_db_json_array() -> *mut c_void;
    fn web_indexed_db_json_array_append(array: *mut c_void, value: *mut c_void);
}

/// A WebIDL::DOMException of LibWeb.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DomException(CppCell);

impl DomException {
    pub(crate) fn create(name: &str, message: &str) -> Self {
        Self::create_with_utf16_message(name, &Utf16String::from_utf8(message))
    }

    fn create_with_utf16_message(name: &str, message: &Utf16String) -> Self {
        // SAFETY: The name and the message are lent for the call, and LibWeb returns a live DOMException.
        Self(unsafe {
            CppCell::from_raw(web_indexed_db_create_dom_exception(
                ak::Span::new(name.as_bytes()),
                message.raw_identity(),
            ))
        })
    }

    /// The operations of requests only throw DOMExceptions.
    pub(crate) fn from_exception(exception: Exception) -> Self {
        match exception {
            Exception::Dom(name, message) => Self::create_with_utf16_message(name, &message),
            Exception::Thrown(throw) => {
                // SAFETY: The value is live while the call reads it.
                let exception = unsafe { web_indexed_db_dom_exception_from_value(throw.value().to_raw()) };
                assert!(!exception.is_null(), "the operation of a request threw a DOMException");
                // SAFETY: LibWeb returns a live DOMException.
                Self(unsafe { CppCell::from_raw(exception) })
            }
            Exception::TypeError(_) | Exception::RangeError(_) => {
                unreachable!("the operation of a request threw a DOMException")
            }
        }
    }

    pub(crate) fn wrapper(&self, cx: &CallContext) -> CppInterface {
        // SAFETY: The realm and the DOMException are live, and LibWeb returns a live wrapper.
        let wrapper = unsafe { web_indexed_db_wrap_dom_exception(cx.realm().as_ptr().cast(), self.0.as_ptr()) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns a wrapper");
        // SAFETY: The wrapper is a live wrapper of a DOMException.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }
}

/// https://storage.spec.whatwg.org/#obtain-a-storage-key, given the relevant settings object of the realm's global
/// object.
pub(crate) fn obtain_a_storage_key(realm: Gc<Realm>) -> Option<StorageKey> {
    unsafe extern "C" fn on_origin(ctx: *mut c_void, ffi_origin: *const FfiOrigin) {
        // SAFETY: ctx is the origin below, and LibWeb lends the origin for the call.
        unsafe { *ctx.cast::<Option<Origin>>() = Some((*ffi_origin).to_origin()) };
    }

    let mut origin: Option<Origin> = None;
    // SAFETY: The realm is live, and the origin outlives the call.
    unsafe { web_indexed_db_obtain_a_storage_key(realm.as_ptr().cast(), (&raw mut origin).cast(), on_origin) };
    origin.map(|origin| StorageKey { origin })
}

pub(crate) fn create_dom_string_list(cx: &CallContext, strings: &[Utf16String]) -> CppInterface {
    let raw_strings: Vec<usize> = strings.iter().map(Utf16String::raw_identity).collect();
    // SAFETY: The realm is live, the strings are lent for the call, and LibWeb returns a live wrapper.
    let wrapper = unsafe {
        web_indexed_db_create_dom_string_list(cx.realm().as_ptr().cast(), raw_strings.as_ptr(), raw_strings.len())
    };
    let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns a wrapper");
    // SAFETY: The wrapper is a live wrapper of a DOMStringList.
    unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
pub(crate) fn structured_serialize_for_storage(
    cx: &CallContext,
    value: Value,
) -> WebIdlResult<StorageSerializationRecord> {
    unsafe extern "C" fn append(bytes: *mut c_void, data: *const u8, length: usize) {
        // SAFETY: The bytes are the Vec below, and LibWeb passes that many bytes.
        unsafe { (*bytes.cast::<Vec<u8>>()).extend_from_slice(ak::slice_from_raw_parts(data, length)) };
    }

    let mut bytes: Vec<u8> = Vec::new();
    // SAFETY: The realm and the value are live, and the bytes outlive the call.
    unsafe {
        web_indexed_db_structured_serialize_for_storage(
            cx.realm().as_ptr().cast(),
            value.to_raw(),
            (&raw mut bytes).cast(),
            append,
        )
    }
    .into_completion()?;
    Ok(Rc::from(bytes))
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
pub(crate) fn structured_deserialize(cx: &CallContext, serialized: &StorageSerializationRecord) -> WebIdlResult<Value> {
    // SAFETY: The realm is live, and the bytes are lent for the call.
    Ok(
        unsafe { web_indexed_db_structured_deserialize(cx.realm().as_ptr().cast(), ak::Span::new(serialized)) }
            .into_completion()?,
    )
}

unsafe extern "C" fn run_function(vm: *mut c_void, cell: *mut c_void) {
    // SAFETY: LibWeb passes the VM and the cell of the function, which it kept alive.
    let (vm, function) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Function::from_cell(Gc::from_non_null(NonNull::new_unchecked(
                cell.cast::<EmbedderCell<FunctionCell>>(),
            ))),
        )
    };
    call_function(vm, function);
}

/// Queues the task with no document and no event loop, as the C++ did.
pub(crate) fn queue_a_task_on_the_database_access_task_source(_: &Vm, steps: Function) {
    // SAFETY: The function is live, and the task keeps it alive until it runs.
    unsafe { web_indexed_db_queue_a_database_task(steps.cell_as_ptr(), run_function) };
}

/// Runs the function from a deferred invocation, as the C++ ran steps "in parallel".
pub(crate) fn run_in_parallel(_: &Vm, steps: Function) {
    // SAFETY: The function is live, and the deferred invocation keeps it alive until it runs.
    unsafe { web_indexed_db_run_in_parallel(steps.cell_as_ptr(), run_function) };
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#main-thread-event-loop
pub(crate) fn main_thread_event_loop() -> CppCell {
    // SAFETY: LibWeb returns the live event loop.
    unsafe { CppCell::from_raw(web_indexed_db_main_thread_event_loop()) }
}

/// Checks the database's pending connection wait after the collection that runs a finalizer, if the database is still
/// alive then. Must not allocate a cell.
pub(crate) fn enqueue_post_gc_task(database: crate::indexed_db::internal::database::DatabaseRef) {
    // SAFETY: The database is live while LibGC finalizes the connection.
    unsafe { web_indexed_db_enqueue_check_pending_connection_wait(database.as_ptr().cast()) };
}

/// Runs the check that enqueue_post_gc_task() enqueued.
///
/// # Safety
///
/// The pointers must be the live VM and the live cell of a database.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_check_pending_connection_wait(vm: *mut c_void, database: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live database.
    let (vm, database) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new_unchecked(database.cast::<EmbedderCell<Database>>())),
        )
    };
    database.check_pending_connection_wait(vm);
}

pub(crate) fn devtools_client_is_active(realm: Gc<Realm>) -> bool {
    // SAFETY: The realm is live.
    unsafe { web_indexed_db_devtools_client_is_active(realm.as_ptr().cast()) }
}

/// A JsonValue of the key, which the caller takes over.
pub(crate) fn json_value_of_key(key: &Key) -> *mut c_void {
    // SAFETY: Each call hands over a new JsonValue, which the array takes over when it is appended.
    unsafe {
        match key.value() {
            _ if key.is_invalid() => web_indexed_db_json_null(),
            KeyValue::Double(value) => web_indexed_db_json_number(*value),
            KeyValue::String(value) => web_indexed_db_json_string(value.raw_identity()),
            KeyValue::Bytes(value) => web_indexed_db_json_binary(ak::Span::new(value)),
            KeyValue::Keys(subkeys) => {
                let array = web_indexed_db_json_array();
                for subkey in subkeys {
                    web_indexed_db_json_array_append(array, json_value_of_key(subkey));
                }
                array
            }
        }
    }
}

pub(crate) fn notify_devtools_of_committed_changes(realm: Gc<Realm>, changes: &TransactionChanges) {
    // SAFETY: LibWeb returns new changes, which the notification takes over.
    let cpp_changes = unsafe { web_indexed_db_create_transaction_changes() };
    let append = |kind: u8, change: &TransactionChange| {
        let key = change
            .key
            .as_ref()
            .map_or(core::ptr::null_mut(), |key| json_value_of_key(key));
        // SAFETY: The changes are live, the names are lent for the call, and the call takes over the key.
        unsafe {
            web_indexed_db_append_transaction_change(
                cpp_changes,
                kind,
                change.database_name.raw_identity(),
                change.object_store_name.raw_identity(),
                key,
            );
        }
    };
    for change in &changes.added {
        append(0, change);
    }
    for change in &changes.changed {
        append(1, change);
    }
    for change in &changes.deleted {
        append(2, change);
    }
    // SAFETY: The realm is live, and the call takes over the changes.
    unsafe { web_indexed_db_notify_devtools(realm.as_ptr().cast(), cpp_changes) };
}
