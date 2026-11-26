/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which Hub hosts a browsing session's IndexedDB backend. Requests arrive as the bytes an agent
//! encoded, and events leave as one buffer of encoded events per client.

use core::ffi::c_void;

use libipc_rust::wire::{Decoder, Encoder};

use crate::indexed_db::backend::inspection::Inspection;
use crate::indexed_db::backend::persistence::{DatabaseRowSink, Persistence, RecordRowSink, Write};
use crate::indexed_db::backend::{Backend, ClientId, Event, Limits, Request};
use crate::rust_panic::abort_on_panic;

/// A browsing session's backend, which C++ owns through the pointer it creates.
pub struct IndexedDbBackend(Backend);

#[unsafe(no_mangle)]
pub extern "C" fn rust_indexed_db_backend_create() -> *mut IndexedDbBackend {
    abort_on_panic(|| Box::into_raw(Box::new(IndexedDbBackend(Backend::new()))))
}

/// Bounds the bytes that every storage key's databases may take together, which the backend holds in memory.
///
/// # Safety
///
/// The backend must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_set_session_limit(backend: *mut IndexedDbBackend, bytes: u64) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend.
        let backend = unsafe { &mut (*backend).0 };
        backend.set_limits(Limits {
            session: bytes,
            ..Limits::default()
        });
    });
}

/// Where a backend that C++ creates keeps its databases between runs: a context, and the functions the backend calls
/// with it. The bytes each function receives are lent for the call.
#[repr(C)]
pub struct IndexedDbPersistenceCallbacks {
    pub context: *mut c_void,
    /// Hands each row of a storage key's databases to `on_database` and then `on_record`, with `sink`, and returns
    /// whether every row could be read; it hands over none if not.
    pub load: unsafe extern "C" fn(
        context: *mut c_void,
        storage_key: *const u8,
        storage_key_length: usize,
        sink: *mut c_void,
        on_database: unsafe extern "C" fn(
            sink: *mut c_void,
            name: *const u8,
            name_length: usize,
            metadata: *const u8,
            metadata_length: usize,
        ),
        on_record: unsafe extern "C" fn(
            sink: *mut c_void,
            database: *const u8,
            database_length: usize,
            store: u64,
            key: *const u8,
            key_length: usize,
            value: *const u8,
            value_length: usize,
            index_keys: *const u8,
            index_keys_length: usize,
        ),
    ) -> bool,
    /// Starts the changes to one database's rows, which apply all at once at `end_write`.
    pub begin_write: unsafe extern "C" fn(
        context: *mut c_void,
        storage_key: *const u8,
        storage_key_length: usize,
        database: *const u8,
        database_length: usize,
    ),
    pub put_metadata: unsafe extern "C" fn(context: *mut c_void, metadata: *const u8, length: usize),
    pub put_record: unsafe extern "C" fn(
        context: *mut c_void,
        store: u64,
        key: *const u8,
        key_length: usize,
        value: *const u8,
        value_length: usize,
        index_keys: *const u8,
        index_keys_length: usize,
    ),
    pub delete_record: unsafe extern "C" fn(context: *mut c_void, store: u64, key: *const u8, key_length: usize),
    pub delete_store: unsafe extern "C" fn(context: *mut c_void, store: u64),
    pub delete_database: unsafe extern "C" fn(context: *mut c_void),
    /// Applies the changes since `begin_write`, all or none, and returns whether it did.
    pub end_write: unsafe extern "C" fn(context: *mut c_void) -> bool,
}

struct CallbackPersistence(IndexedDbPersistenceCallbacks);

struct LoadSink<'a> {
    on_database: &'a mut DatabaseRowSink<'a>,
    on_record: &'a mut RecordRowSink<'a>,
}

unsafe extern "C" fn sink_database(
    sink: *mut c_void,
    name: *const u8,
    name_length: usize,
    metadata: *const u8,
    metadata_length: usize,
) {
    // SAFETY: The sink is the one load() lent, and the bytes are lent for the call.
    unsafe {
        let sink = &mut *sink.cast::<LoadSink<'_>>();
        (sink.on_database)(
            ak::slice_from_raw_parts(name, name_length),
            ak::slice_from_raw_parts(metadata, metadata_length),
        );
    }
}

#[allow(clippy::too_many_arguments, reason = "each slice is a pointer and a length")]
unsafe extern "C" fn sink_record(
    sink: *mut c_void,
    database: *const u8,
    database_length: usize,
    store: u64,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
    index_keys: *const u8,
    index_keys_length: usize,
) {
    // SAFETY: The sink is the one load() lent, and the bytes are lent for the call.
    unsafe {
        let sink = &mut *sink.cast::<LoadSink<'_>>();
        (sink.on_record)(
            ak::slice_from_raw_parts(database, database_length),
            store,
            ak::slice_from_raw_parts(key, key_length),
            ak::slice_from_raw_parts(value, value_length),
            ak::slice_from_raw_parts(index_keys, index_keys_length),
        );
    }
}

impl Persistence for CallbackPersistence {
    fn load(
        &mut self,
        storage_key: &str,
        on_database: &mut DatabaseRowSink<'_>,
        on_record: &mut RecordRowSink<'_>,
    ) -> bool {
        let mut sink = LoadSink { on_database, on_record };
        // SAFETY: The context is the caller's, and the sink and the storage key are live for the call.
        unsafe {
            (self.0.load)(
                self.0.context,
                storage_key.as_ptr(),
                storage_key.len(),
                (&raw mut sink).cast(),
                sink_database,
                sink_record,
            )
        }
    }

    fn write(&mut self, storage_key: &str, database: &[u8], writes: Vec<Write>) -> bool {
        let callbacks = &self.0;
        let context = callbacks.context;
        // SAFETY: The context is the caller's, and every slice is live for its call.
        unsafe {
            (callbacks.begin_write)(
                context,
                storage_key.as_ptr(),
                storage_key.len(),
                database.as_ptr(),
                database.len(),
            );
            for write in &writes {
                match write {
                    Write::Metadata(metadata) => (callbacks.put_metadata)(context, metadata.as_ptr(), metadata.len()),
                    Write::Record {
                        store,
                        key,
                        value,
                        index_keys,
                    } => (callbacks.put_record)(
                        context,
                        *store,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                        index_keys.as_ptr(),
                        index_keys.len(),
                    ),
                    Write::DeleteRecord { store, key } => {
                        (callbacks.delete_record)(context, *store, key.as_ptr(), key.len());
                    }
                    Write::DeleteStore(store) => (callbacks.delete_store)(context, *store),
                    Write::DeleteDatabase => (callbacks.delete_database)(context),
                }
            }
            (callbacks.end_write)(context)
        }
    }
}

/// A backend that keeps its databases between runs through the callbacks, whose context must outlive it.
#[unsafe(no_mangle)]
pub extern "C" fn rust_indexed_db_backend_create_persistent(
    callbacks: IndexedDbPersistenceCallbacks,
) -> *mut IndexedDbBackend {
    abort_on_panic(|| {
        Box::into_raw(Box::new(IndexedDbBackend(Backend::with_persistence(Box::new(
            CallbackPersistence(callbacks),
        )))))
    })
}

/// # Safety
///
/// `backend` must come from `rust_indexed_db_backend_create()`, and not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_destroy(backend: *mut IndexedDbBackend) {
    abort_on_panic(|| {
        // SAFETY: The caller hands over a backend it created.
        drop(unsafe { Box::from_raw(backend) });
    });
}

/// Binds a client the caller numbered to a storage key, which C++ derived from the agent's environment.
///
/// # Safety
///
/// `backend` must be live, and `storage_key` must lend `length` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_connect_client(
    backend: *mut IndexedDbBackend,
    client: ClientId,
    storage_key: *const u8,
    length: usize,
) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend and `length` bytes.
        let backend = unsafe { &mut *backend };
        let bytes = unsafe { ak::slice_from_raw_parts(storage_key, length) };
        backend
            .0
            .connect_client(client, String::from_utf8_lossy(bytes).into_owned());
    });
}

/// # Safety
///
/// `backend` must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_disconnect_client(backend: *mut IndexedDbBackend, client: ClientId) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend.
        unsafe { &mut *backend }.0.disconnect_client(client);
    });
}

/// Hands the backend a request that a client encoded, and returns false for bytes that don't decode as one, which
/// marks the agent that sent them as misbehaving.
///
/// # Safety
///
/// `backend` must be live, and `bytes` must lend `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_handle(
    backend: *mut IndexedDbBackend,
    client: ClientId,
    bytes: *const u8,
    length: usize,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend and `length` bytes.
        let backend = unsafe { &mut *backend };
        let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
        let Ok(request) = Decoder::new(bytes, Vec::new()).decode::<Request>() else {
            return false;
        };
        backend.0.handle(client, request);
        true
    })
}

/// Hands `callback` each storage key whose databases changed in this run at or after the time, in milliseconds since
/// the epoch.
///
/// # Safety
///
/// `backend` must be live, and `callback` must not reenter the backend.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_storage_keys_written_since(
    backend: *mut IndexedDbBackend,
    since: u64,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, storage_key: *const u8, length: usize),
) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend.
        let backend = unsafe { &*backend };
        for storage_key in backend.0.storage_keys_written_since(since) {
            // SAFETY: The callback borrows the bytes for the call.
            unsafe { callback(context, storage_key.as_ptr(), storage_key.len()) };
        }
    });
}

/// Deletes every database of the storage key, as when the user clears the site's data, and returns whether every
/// database's rows went.
///
/// # Safety
///
/// `backend` must be live, and the storage key `length` bytes of UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_clear_storage_key(
    backend: *mut IndexedDbBackend,
    storage_key: *const u8,
    length: usize,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend and `length` bytes of UTF-8.
        let (backend, storage_key) = unsafe {
            (
                &mut *backend,
                core::str::from_utf8_unchecked(ak::slice_from_raw_parts(storage_key, length)),
            )
        };
        backend.0.clear_storage_key(storage_key)
    })
}

/// Hands each client's events since the last call to `callback`, encoded as one list per client, in the order they
/// happened.
///
/// # Safety
///
/// `backend` must be live, and `callback` must not reenter the backend.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_take_events(
    backend: *mut IndexedDbBackend,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, client: ClientId, bytes: *const u8, length: usize),
) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend.
        let backend = unsafe { &mut *backend };
        let mut per_client: Vec<(ClientId, Vec<Event>)> = Vec::new();
        for (client, event) in backend.0.take_events() {
            match per_client.iter_mut().find(|(entry, _)| *entry == client) {
                Some((_, events)) => events.push(event),
                None => per_client.push((client, vec![event])),
            }
        }
        for (client, events) in per_client {
            let mut encoder = Encoder::new();
            if encoder.encode(&events).is_err() {
                continue;
            }
            let (bytes, _) = encoder.finish();
            // SAFETY: The callback borrows the bytes for the call.
            unsafe { callback(context, client, bytes.as_ptr(), bytes.len()) };
        }
    });
}

/// Answers an inspection that a client encoded, handing the encoded result to `callback`, and returns false for bytes
/// that don't decode as one.
///
/// # Safety
///
/// `backend` must be live, `bytes` must lend `length` bytes, and `callback` must not reenter the backend.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_indexed_db_backend_inspect(
    backend: *mut IndexedDbBackend,
    client: ClientId,
    bytes: *const u8,
    length: usize,
    context: *mut c_void,
    callback: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, length: usize),
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller lends a live backend and `length` bytes.
        let backend = unsafe { &mut *backend };
        let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
        let Ok(inspection) = Decoder::new(bytes, Vec::new()).decode::<Inspection>() else {
            return false;
        };
        let result = backend.0.inspect(client, inspection);
        let mut encoder = Encoder::new();
        if encoder.encode(&result).is_err() {
            return false;
        }
        let (bytes, _) = encoder.finish();
        // SAFETY: The callback borrows the bytes for the call.
        unsafe { callback(context, bytes.as_ptr(), bytes.len()) };
        true
    })
}
