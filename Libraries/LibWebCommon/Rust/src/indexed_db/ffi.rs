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
