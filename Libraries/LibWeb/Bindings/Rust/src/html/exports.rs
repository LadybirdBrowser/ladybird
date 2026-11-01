/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates and reads the ImageData, ImageBitmap and MessagePort objects that it
//! handles, which Libraries/LibWeb/HTML/ImageData.cpp, Libraries/LibWeb/HTML/ImageBitmap.cpp and
//! Libraries/LibWeb/HTML/MessagePort.cpp declare. A cell is the cell of an object of the interface that the name of
//! the function says. Libraries/LibWeb/HTML/BroadcastChannel.cpp hands the BroadcastChannel objects the messages that
//! other processes post.

use core::ffi::{c_int, c_void};
use core::ptr::NonNull;

use ak::Utf16String;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Gc, MarkedVec, Object, RawNativeFunctionResult, Value, Vm};
use liburl_rust::FfiOrigin;

use super::broadcast_channel::{BROADCAST_CHANNEL_MESSAGE_CLASS, BroadcastChannel, BroadcastChannelMessage};
use super::image_bitmap::ImageBitmap;
use super::image_data::ImageData;
use super::message_port::MessagePort;
use super::storage::{Storage, StorageType};
use super::storage_event::{StorageEvent, StorageEventInit};
use crate::bindings::CallContext;
use crate::bindings::conversions::UsvString;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::CppEventTarget;
use crate::bindings::interface::{Interface, Ref, cell_from_abi, cell_into_abi, realm_arguments, wrap};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::dom::event::EventInit;
use crate::html::structured_serialize::cpp::TransferDataDecoder;
use crate::html::structured_serialize::message_record::MessageRecord;
use crate::html::structured_serialize::transfer_data_holder::TransferDataHolder;
use crate::storage_api::storage_area_mirror::StorageEndpointType;
use crate::storage_api::storage_bottle::{obtain_a_local_storage_bottle_map, obtain_a_session_storage_bottle_map};
use crate::storage_api::storage_key::StorageKey;

/// The implementation of the object whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `cell` must be the live cell of an object of the interface, and it must outlive `'a`.
unsafe fn object_from_abi<'a, T: Interface>(cell: *mut c_void) -> &'a T {
    // SAFETY: The caller passes a live cell of the interface.
    let object: Ref<T> = unsafe { cell_from_abi(cell) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*object.as_ptr() }
}

/// Creates an ImageData of the size, with transparent black pixels and the sRGB color space, in the realm, and stores
/// its cell, or throws what initializing an ImageData throws.
///
/// # Safety
///
/// `vm` must be the live VM, and `realm` a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_create(
    vm: *mut c_void,
    realm: *mut c_void,
    width: u32,
    height: u32,
    out_image_data: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    let result = ImageData::create(&mut cx, width, height, None, None).map(|image_data| {
        *out_image_data = cell_into_abi(image_data);
        Value::UNDEFINED
    });
    RawNativeFunctionResult::from_completion(result.map_err(|exception| exception.throw(&cx)))
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_width(image_data: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.size().0
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_height(image_data: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.size().1
}

/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_is_detached(image_data: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.is_detached()
}

/// The colorSpace of the ImageData, as the underlying value of a Web::Bindings::PredefinedColorSpace.
///
/// # Safety
///
/// `image_data` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_color_space(image_data: *mut c_void) -> u8 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageData>(image_data) }.predefined_color_space() as u8
}

/// The first byte of the pixels of the ImageData, or null if its buffer is detached or its data has no room for them.
///
/// # Safety
///
/// `image_data` must be a live cell. The pixels are valid until script runs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_data_pixels(image_data: *mut c_void) -> *mut u8 {
    // SAFETY: The caller passes a live cell.
    let image_data = unsafe { object_from_abi::<ImageData>(image_data) };
    image_data.pixels().map_or(core::ptr::null_mut(), NonNull::as_ptr)
}

/// `frame` is a Gfx::DecodedImageFrame that the caller lends for the call, which the ImageBitmap keeps a copy of as its
/// bitmap data, or null for none.
///
/// # Safety
///
/// `frame` must be a live frame or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_create(vm: &Vm, frame: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live frame.
    let frame = (!frame.is_null()).then(|| unsafe { ImageFrameHandle::retain(frame) });
    cell_into_abi(ImageBitmap::create(vm, frame))
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_width(image_bitmap: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.width_of_bitmap()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_height(image_bitmap: *mut c_void) -> u32 {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.height_of_bitmap()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_is_detached(image_bitmap: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.is_detached()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_close(image_bitmap: *mut c_void) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.close_bitmap();
}

/// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
///
/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_origin_clean(image_bitmap: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.origin_clean()
}

/// # Safety
///
/// `image_bitmap` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_set_origin_clean(image_bitmap: *mut c_void, origin_clean: bool) {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.set_origin_clean(origin_clean);
}

/// The Gfx::DecodedImageFrame of the bitmap data of the ImageBitmap, or null if it has none.
///
/// # Safety
///
/// `image_bitmap` must be a live cell. The frame is valid until the bitmap data is unset, by close() or a transfer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_frame(image_bitmap: *mut c_void) -> *const c_void {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<ImageBitmap>(image_bitmap) }.raw_frame()
}

/// # Safety
///
/// `vm` must be the live VM, and `realm` a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_create(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(MessagePort::create(vm, realm))
}

/// Entangles the two ports, https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
///
/// # Safety
///
/// `vm` must be the live VM, and `port` and `remote_port` live cells of MessagePorts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_entangle(vm: *mut c_void, port: *mut c_void, remote_port: *mut c_void) {
    // SAFETY: The caller passes the live VM and live cells of MessagePorts.
    let (vm, port, remote_port) = unsafe {
        (
            &*vm.cast::<Vm>(),
            cell_from_abi::<MessagePort>(port),
            cell_from_abi::<MessagePort>(remote_port),
        )
    };
    MessagePort::entangle(vm, port, remote_port);
}

/// # Safety
///
/// `port` must be a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_entangled_port(port: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes a live cell of a MessagePort.
    let port = unsafe { cell_from_abi::<MessagePort>(port) };
    port.entangled_port().map_or(core::ptr::null_mut(), cell_into_abi)
}

/// Sets the port's message event target, https://html.spec.whatwg.org/multipage/web-messaging.html#message-event-target
///
/// # Safety
///
/// `port` must be a live cell of a MessagePort, and `event_target` a live EventTarget.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_set_message_event_target(port: *mut c_void, event_target: *mut c_void) {
    // SAFETY: The caller passes a live cell of a MessagePort and a live EventTarget.
    let (port, event_target) = unsafe {
        (
            cell_from_abi::<MessagePort>(port),
            CppEventTarget::from_raw(event_target),
        )
    };
    port.set_message_event_target(event_target);
}

/// postMessage(message, transfer) on the port, https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `port` a live cell of a MessagePort. The message must be live,
/// and the transfer list `count` live objects.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_post_message(
    vm: *mut c_void,
    realm: *mut c_void,
    port: *mut c_void,
    message: u64,
    transfer: *const *mut c_void,
    count: usize,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let mut cx = CallContext::new(vm, realm);
    // SAFETY: The caller passes a live cell of a MessagePort and a live value.
    let (port, message) = unsafe { (cell_from_abi::<MessagePort>(port), Value::from_raw(message)) };
    let transfer_list = MarkedVec::with_capacity(vm, count);
    for index in 0..count {
        // SAFETY: The caller passes `count` live objects.
        transfer_list.push(unsafe {
            Gc::from_non_null(NonNull::new((*transfer.add(index)).cast::<Object>()).expect("C++ passes objects"))
        });
    }
    let result = MessagePort::post_message(&port, &mut cx, message, transfer_list);
    RawNativeFunctionResult::from_completion(
        result
            .map(|()| Value::UNDEFINED)
            .map_err(|exception| exception.throw(&cx)),
    )
}

/// # Safety
///
/// `vm` must be the live VM, and `port` a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_enable(vm: *mut c_void, port: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live cell of a MessagePort.
    let (vm, port) = unsafe { (&*vm.cast::<Vm>(), cell_from_abi::<MessagePort>(port)) };
    MessagePort::enable(port, vm);
}

/// The close() method steps of the port, https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-close
///
/// # Safety
///
/// `port` must be a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_close(port: *mut c_void) {
    // SAFETY: The caller passes a live cell of a MessagePort.
    unsafe { cell_from_abi::<MessagePort>(port) }.close_steps();
}

/// # Safety
///
/// `port` must be a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_discard_pending_messages(port: *mut c_void) {
    // SAFETY: The caller passes a live cell of a MessagePort.
    unsafe { cell_from_abi::<MessagePort>(port) }.discard_pending_messages();
}

/// # Safety
///
/// `port` must be a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_pending_outgoing_message_count(port: *mut c_void) -> usize {
    // SAFETY: The caller passes a live cell of a MessagePort.
    unsafe { cell_from_abi::<MessagePort>(port) }.pending_outgoing_message_count()
}

/// # Safety
///
/// `port` must be a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_fail_next_transfer_for_testing(port: *mut c_void) {
    // SAFETY: The caller passes a live cell of a MessagePort.
    unsafe { cell_from_abi::<MessagePort>(port) }.fail_next_transfer_for_testing();
}

/// The transfer steps of the MessagePort, which C++ runs outside StructuredSerializeWithTransfer, into a data holder
/// that holds no type, whose carrier it hands over.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `port` a live cell of a MessagePort.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_transfer_steps(
    vm: *mut c_void,
    realm: *mut c_void,
    port: *mut c_void,
    out_data_holder: &mut *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM, a live realm and a live cell of a MessagePort.
    let ((vm, realm), port) = unsafe { (realm_arguments(vm, realm), cell_from_abi::<MessagePort>(port)) };
    let mut cx = CallContext::new(vm, realm);
    let data_holder = TransferDataHolder::new();
    let result = MessagePort::transfer_steps(&port, &mut cx, &mut TransferDataWriter::new(&data_holder, realm, None));
    if result.is_ok() {
        *out_data_holder = data_holder.into_carrier().into_raw();
    }
    RawNativeFunctionResult::from_completion(
        result
            .map(|()| Value::UNDEFINED)
            .map_err(|exception| exception.throw(&cx)),
    )
}

/// The transfer-receiving steps of a MessagePort that C++ runs outside StructuredDeserializeWithTransfer, which return
/// the wrapper of the new MessagePort.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `data_holder` an HTML::TransferDataDecoder that C++ lends for
/// the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_transfer_receiving_steps(
    vm: *mut c_void,
    realm: *mut c_void,
    data_holder: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller passes the live VM and a live realm, and lends the data holder for the call.
    let ((vm, realm), decoder) = unsafe { (realm_arguments(vm, realm), TransferDataDecoder::from_raw(data_holder)) };
    let mut cx = CallContext::new(vm, realm);
    // SAFETY: The caller lends the data holder for the call.
    let mut data_holder = unsafe { TransferDataReader::new(decoder, realm) };
    let result = MessagePort::transfer_receiving_steps(&mut cx, &mut data_holder);
    RawNativeFunctionResult::from_completion(
        result
            .map(|port| Value::from_object(wrap(&cx, port)))
            .map_err(|exception| exception.throw(&cx)),
    )
}

/// # Safety
///
/// `vm` must be the live VM, and `realm` a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_disentangle_ports_of_realm(vm: *mut c_void, realm: *mut c_void) {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    MessagePort::disentangle_ports_of_realm(vm, realm);
}

/// Steps 6 to 9 of BroadcastChannel's postMessage(), for a message that another process posted, whose channel name
/// C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, the origins and the bytes lent, `channel_name` an owned AK::Utf16String, and
/// `shared_memory` and `shared_memory_sizes` arrays of `shared_memory_count` descriptors that are open for the call and
/// their sizes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_broadcast_channel_deliver_message_locally(
    vm: *mut c_void,
    storage_key_origin: *const FfiOrigin,
    channel_name: usize,
    source_origin: *const FfiOrigin,
    serialized_message: ak::Span<'_, u8>,
    shared_memory: *const c_int,
    shared_memory_sizes: *const usize,
    shared_memory_count: usize,
) {
    // SAFETY: The caller passes the live VM, lends the origins and the descriptors, and hands over the name.
    let (vm, storage_key, channel_name, source_origin, shared_memory, shared_memory_sizes) = unsafe {
        (
            &*vm.cast::<Vm>(),
            StorageKey {
                origin: (*storage_key_origin).to_origin(),
            },
            Utf16String::from_raw_owned(channel_name),
            (*source_origin).to_origin(),
            ak::slice_from_raw_parts(shared_memory, shared_memory_count),
            ak::slice_from_raw_parts(shared_memory_sizes, shared_memory_count),
        )
    };
    let message = BROADCAST_CHANNEL_MESSAGE_CLASS.allocate(vm, |defer_gc| BroadcastChannelMessage {
        storage_key,
        channel_name,
        source_origin,
        // SAFETY: The caller keeps the descriptors open for the call.
        serialized: unsafe {
            MessageRecord::received(
                defer_gc,
                serialized_message.as_slice(),
                shared_memory,
                shared_memory_sizes,
            )
        },
    });
    BroadcastChannel::deliver_message_locally(vm, message, None);
}

/// A new Storage object of the type, 0 for "local" and 1 for "session", for the Window of the realm, whose map is a new
/// storage proxy map over the bottle for the realm's settings object: steps 2 to 4 of the localStorage and
/// sessionStorage getters. Null when obtaining the bottle map fails.
///
/// # Safety
///
/// The pointers must be the live VM and the live realm of a Window.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_create_storage(
    vm: *mut c_void,
    realm: *mut c_void,
    storage_type: u8,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let storage_type = StorageType::from_abi(storage_type);
    let environment = relevant_settings_object(realm.global_object());
    let map = match storage_type {
        StorageType::Local => obtain_a_local_storage_bottle_map(&environment, StorageEndpointType::LocalStorage),
        StorageType::Session => obtain_a_session_storage_bottle_map(&environment, StorageEndpointType::SessionStorage),
    };
    map.map_or(core::ptr::null_mut(), |map| {
        cell_into_abi(Storage::create(vm, realm, storage_type, map))
    })
}

/// Loads the map of the Storage object now rather than on first access, so changes other processes make start
/// arriving.
///
/// # Safety
///
/// `storage` must be the live cell of a Storage object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_storage_connect(storage: *mut c_void) {
    // SAFETY: The caller passes a live Storage object.
    unsafe { object_from_abi::<Storage>(storage) }.map().connect();
}

/// Calls `on_item` with each key of the map of the Storage object and its value, raw AK::Utf16Strings that Rust lends
/// for the call.
///
/// # Safety
///
/// `storage` must be the live cell of a Storage object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_storage_for_each_item(
    storage: *mut c_void,
    ctx: *mut c_void,
    on_item: unsafe extern "C" fn(*mut c_void, usize, usize),
) {
    // SAFETY: The caller passes a live Storage object.
    let map = unsafe { object_from_abi::<Storage>(storage) }.map();
    for key in map.keys() {
        if let Some(value) = map.get(&key) {
            // SAFETY: The strings outlive the call.
            unsafe { on_item(ctx, key.raw_identity(), value.raw_identity()) };
        }
    }
}

/// The Event of a new StorageEvent in the realm whose type is the empty string and whose attributes are their
/// defaults, for createEvent().
///
/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_create_storage_event(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    let event_init = StorageEventInit {
        parent: EventInit {
            bubbles: false,
            cancelable: false,
            composed: false,
        },
        key: None,
        old_value: None,
        new_value: None,
        url: UsvString::from_utf8(""),
        storage_area: None,
    };
    StorageEvent::create(vm, realm, &Utf16String::from_utf8(""), event_init)
        .event()
        .as_ptr()
}
