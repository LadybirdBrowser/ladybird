/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI of the Storage Standard's interfaces and storage bottles implemented in Rust, which LibWeb's C++ code
//! calls. Libraries/LibWeb/StorageAPI/StorageAreaMirrorClient.cpp connects the mirrors of the bottles to the process
//! that owns the storage areas, and hands them what it relays.

use core::ffi::c_void;
use core::ptr::NonNull;
use std::rc::Rc;

use ak::Utf16String;
use libjs_rust::api::Vm;

use super::storage_area_mirror::{
    RemoteChanges, Requester, StorageAreaConnection, StorageAreaMirror, StorageAreaMirrorClient, StorageEndpointType,
    set_storage_area_mirror_client,
};
use super::storage_manager::StorageManager;
use crate::bindings::interface::{cell_into_abi, realm_arguments};
use crate::html::storage::{StorageType, dispatch_storage_events};

unsafe extern "C" {
    fn web_storage_api_storage_area_mirror_client_connect(
        client: *mut c_void,
        page_id: u64,
        endpoint_type: u8,
        environment: *mut c_void,
        connection: *mut c_void,
        on_connection: unsafe extern "C" fn(*mut c_void, bool, u64, *const usize, *const usize, usize, u64),
    );
    fn web_storage_api_storage_area_mirror_client_disconnect(client: *mut c_void, id: u64);
    fn web_storage_api_storage_area_mirror_client_set_item(
        client: *mut c_void,
        id: u64,
        key: usize,
        value: usize,
        document_url: ak::Span<'_, u8>,
    );
    fn web_storage_api_storage_area_mirror_client_remove_item(
        client: *mut c_void,
        id: u64,
        key: usize,
        document_url: ak::Span<'_, u8>,
    );
    fn web_storage_api_storage_area_mirror_client_clear(client: *mut c_void, id: u64, document_url: ak::Span<'_, u8>);
}

/// A new StorageManager in the realm, the StorageManager object of the realm's settings object.
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_storage_api_create_storage_manager(
    vm: *mut c_void,
    realm: *mut c_void,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(StorageManager::create(vm, realm))
}

/// The StorageAPI::StorageAreaMirrorClient that LibWeb's C++ code connects the mirrors to.
struct CppStorageAreaMirrorClient(NonNull<c_void>);

/// Strings that C++ hands over, as raw AK::Utf16Strings.
///
/// # Safety
///
/// `raws` must be null or point to `count` raw strings that C++ hands over.
unsafe fn adopt_strings(raws: *const usize, count: usize) -> Vec<Utf16String> {
    if count == 0 {
        return Vec::new();
    }
    // SAFETY: The caller passes count raw strings.
    let raws = unsafe { ak::slice_from_raw_parts(raws, count) };
    // SAFETY: The caller hands over every string.
    raws.iter()
        .map(|&raw| unsafe { Utf16String::from_raw_owned(raw) })
        .collect()
}

unsafe extern "C" fn on_connection(
    connection: *mut c_void,
    has_id: bool,
    id: u64,
    keys: *const usize,
    values: *const usize,
    count: usize,
    usage: u64,
) {
    // SAFETY: connection is the connection of the caller, and LibWeb hands over count keys and as many values.
    unsafe {
        *connection.cast::<StorageAreaConnection>() = StorageAreaConnection {
            id: has_id.then_some(id),
            keys: adopt_strings(keys, count),
            values: adopt_strings(values, count),
            usage,
        };
    }
}

impl StorageAreaMirrorClient for CppStorageAreaMirrorClient {
    fn connect_to_storage_area(
        &self,
        requester: &Requester<'_>,
        endpoint_type: StorageEndpointType,
    ) -> StorageAreaConnection {
        let mut connection = StorageAreaConnection::default();
        // SAFETY: The client and the requester's settings object are live, and the connection outlives the call.
        unsafe {
            web_storage_api_storage_area_mirror_client_connect(
                self.0.as_ptr(),
                requester.page_id,
                endpoint_type as u8,
                requester.environment,
                (&raw mut connection).cast(),
                on_connection,
            );
        }
        connection
    }

    fn disconnect_from_storage_area(&self, id: u64) {
        // SAFETY: The client is live.
        unsafe { web_storage_api_storage_area_mirror_client_disconnect(self.0.as_ptr(), id) };
    }

    fn set_storage_item(&self, id: u64, key: &Utf16String, value: &Utf16String, document_url: &str) {
        // SAFETY: The client is live, and the strings and the URL are lent for the call.
        unsafe {
            web_storage_api_storage_area_mirror_client_set_item(
                self.0.as_ptr(),
                id,
                key.raw_identity(),
                value.raw_identity(),
                ak::Span::new(document_url.as_bytes()),
            );
        }
    }

    fn remove_storage_item(&self, id: u64, key: &Utf16String, document_url: &str) {
        // SAFETY: The client is live, and the key and the URL are lent for the call.
        unsafe {
            web_storage_api_storage_area_mirror_client_remove_item(
                self.0.as_ptr(),
                id,
                key.raw_identity(),
                ak::Span::new(document_url.as_bytes()),
            );
        }
    }

    fn clear_storage_area(&self, id: u64, document_url: &str) {
        // SAFETY: The client is live, and the URL is lent for the call.
        unsafe {
            web_storage_api_storage_area_mirror_client_clear(
                self.0.as_ptr(),
                id,
                ak::Span::new(document_url.as_bytes()),
            );
        }
    }
}

/// Connects the mirrors to the client, or leaves every mirror detached for null.
///
/// # Safety
///
/// `client` must be null or a StorageAPI::StorageAreaMirrorClient that lives until it is replaced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_storage_api_set_storage_area_mirror_client(client: *mut c_void) {
    set_storage_area_mirror_client(
        NonNull::new(client)
            .map(|client| Rc::new(CppStorageAreaMirrorClient(client)) as Rc<dyn StorageAreaMirrorClient>),
    );
}

// Every change another party made to a mirrored map calls for the storage events the spec's broadcast fires, on every
// Storage object here that shares the map.
fn dispatch(vm: &Vm, changes: Option<RemoteChanges>) {
    let Some(RemoteChanges { mirror, changes }) = changes else {
        return;
    };
    let storage_type = match mirror.endpoint_type() {
        StorageEndpointType::SessionStorage => StorageType::Session,
        _ => StorageType::Local,
    };
    for change in changes {
        dispatch_storage_events(
            vm,
            storage_type,
            mirror.storage_key(),
            mirror.session_page(),
            change.key,
            change.old_value,
            change.new_value,
            &change.url,
            None,
        );
    }
}

/// The change another process made to the storage area, with the strings, raw AK::Utf16Strings that C++ hands over,
/// and the URL, which C++ lends.
///
/// # Safety
///
/// The VM must be live, and every string that the flag next to it says is there must be one that C++ hands over.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments, reason = "a change has these parts")]
pub unsafe extern "C" fn web_rust_storage_api_storage_area_did_change(
    vm: *mut c_void,
    id: u64,
    has_key: bool,
    key: usize,
    has_old_value: bool,
    old_value: usize,
    has_new_value: bool,
    new_value: usize,
    url: ak::Span<'_, u8>,
) {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    // SAFETY: The caller hands over the strings that the flags say are there.
    let string = |is_there: bool, raw: usize| is_there.then(|| unsafe { Utf16String::from_raw_owned(raw) });
    let changes = StorageAreaMirror::did_change(
        id,
        string(has_key, key),
        string(has_old_value, old_value),
        string(has_new_value, new_value),
        String::from_utf8_lossy(url.as_slice()).into_owned(),
    );
    dispatch(vm, changes);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_storage_api_storage_area_did_acknowledge_write(id: u64) {
    StorageAreaMirror::did_acknowledge_write(id);
}

/// The owner refused the oldest write it hasn't answered, and its map is the keys and the values, raw
/// AK::Utf16Strings that C++ hands over.
///
/// # Safety
///
/// The VM must be live, and `keys` and `values` must each point to `count` strings that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_storage_api_storage_area_did_refuse_write(
    vm: *mut c_void,
    id: u64,
    keys: *const usize,
    values: *const usize,
    count: usize,
) {
    // SAFETY: The caller passes the live VM and hands over the strings.
    let (vm, keys, values) = unsafe {
        (
            &*vm.cast::<Vm>(),
            adopt_strings(keys, count),
            adopt_strings(values, count),
        )
    };
    dispatch(vm, StorageAreaMirror::did_refuse_write(id, keys, values));
}

/// The owner's map is now the keys and the values, raw AK::Utf16Strings that C++ hands over.
///
/// # Safety
///
/// The VM must be live, and `keys` and `values` must each point to `count` strings that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_storage_api_storage_area_did_reset(
    vm: *mut c_void,
    id: u64,
    keys: *const usize,
    values: *const usize,
    count: usize,
) {
    // SAFETY: The caller passes the live VM and hands over the strings.
    let (vm, keys, values) = unsafe {
        (
            &*vm.cast::<Vm>(),
            adopt_strings(keys, count),
            adopt_strings(values, count),
        )
    };
    dispatch(vm, StorageAreaMirror::did_reset(id, keys, values));
}
