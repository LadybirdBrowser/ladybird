/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The link of the thread's agent to the backend that holds its databases, in Hub. Each environment that uses IndexedDB
//! connects a client of its own, which Hub binds to the environment's storage key. Requests go out as bytes; the events
//! for a client come back as bytes, and go to the agent's half of the algorithms.

use core::cell::Cell;
use core::ffi::c_void;

use libipc_rust::wire::{Decoder, Encoder};
use libjs_rust::api::Vm;
use libwebcommon_rust::indexed_db::backend::inspection::{Inspection, InspectionResult};
use libwebcommon_rust::indexed_db::backend::{Event, Request};

use crate::indexed_db::remote::{forget_idle_realms, handle_event};

unsafe extern "C" {
    fn web_indexed_db_client_connect(
        client: *mut c_void,
        page_id: u64,
        environment: *mut c_void,
        client_id: *mut u64,
    ) -> bool;
    fn web_indexed_db_client_request(client: *mut c_void, client_id: u64, bytes: *const u8, length: usize);
    fn web_indexed_db_client_disconnect(client: *mut c_void, client_id: u64);
    fn web_indexed_db_client_inspect(
        client: *mut c_void,
        client_id: u64,
        bytes: *const u8,
        length: usize,
        context: *mut c_void,
        on_result: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    );
}

thread_local! {
    static CLIENT: Cell<*mut c_void> = const { Cell::new(core::ptr::null_mut()) };
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_indexed_db_set_client(client: *mut c_void) {
    CLIENT.set(client);
}

/// # Safety
///
/// `bytes` must lend `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_indexed_db_did_receive_events(
    vm: &Vm,
    client_id: u64,
    bytes: *const u8,
    length: usize,
) {
    // SAFETY: The caller lends `length` bytes.
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
    let Ok(events) = Decoder::new(bytes, Vec::new()).decode::<Vec<Event>>() else {
        return;
    };
    for event in events {
        handle_event(vm, client_id, event);
    }
    forget_idle_realms(vm);
}

/// Connects a client for an environment, or returns none when the thread has no link or Hub found no storage key.
pub(crate) fn connect(page_id: u64, environment: *mut c_void) -> Option<u64> {
    let client = CLIENT.get();
    if client.is_null() {
        return None;
    }
    let mut client_id = 0;
    // SAFETY: The link outlives the thread's agent, and the environment is live for the call.
    if !unsafe { web_indexed_db_client_connect(client, page_id, environment, &raw mut client_id) } {
        return None;
    }
    Some(client_id)
}

pub(crate) fn send(client_id: u64, request: &Request) {
    let client = CLIENT.get();
    if client.is_null() {
        return;
    }
    let mut encoder = Encoder::new();
    if encoder.encode(request).is_err() {
        return;
    }
    let (bytes, _) = encoder.finish();
    // SAFETY: The link outlives the thread's agent, and the bytes are live for the call.
    unsafe { web_indexed_db_client_request(client, client_id, bytes.as_ptr(), bytes.len()) };
}

pub(crate) fn disconnect(client_id: u64) {
    // NB: A client that goes as its thread exits finds no link to disconnect through.
    let Ok(client) = CLIENT.try_with(Cell::get) else {
        return;
    };
    if client.is_null() {
        return;
    }
    // SAFETY: The link outlives the thread's agent.
    unsafe { web_indexed_db_client_disconnect(client, client_id) };
}

/// Asks the backend what the developer tools show of a client's databases, and waits for the answer.
pub(crate) fn inspect(client_id: u64, inspection: &Inspection) -> Option<InspectionResult> {
    unsafe extern "C" fn on_result(context: *mut c_void, bytes: *const u8, length: usize) {
        // SAFETY: The context is the result below, and the bytes are lent for the call.
        let (result, bytes) = unsafe {
            (
                &mut *context.cast::<Option<InspectionResult>>(),
                ak::slice_from_raw_parts(bytes, length),
            )
        };
        *result = Decoder::new(bytes, Vec::new()).decode::<InspectionResult>().ok();
    }

    let client = CLIENT.get();
    if client.is_null() {
        return None;
    }
    let mut encoder = Encoder::new();
    encoder.encode(inspection).ok()?;
    let (bytes, _) = encoder.finish();
    let mut result: Option<InspectionResult> = None;
    // SAFETY: The link outlives the thread's agent, and the bytes and the result are live for the call.
    unsafe {
        web_indexed_db_client_inspect(
            client,
            client_id,
            bytes.as_ptr(),
            bytes.len(),
            (&raw mut result).cast(),
            on_result,
        );
    }
    result
}
