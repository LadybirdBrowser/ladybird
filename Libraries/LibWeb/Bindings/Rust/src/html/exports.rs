/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWeb's C++ creates and reads the ImageData, ImageBitmap and MessagePort objects that it
//! handles, which Libraries/LibWeb/HTML/ImageData.cpp, Libraries/LibWeb/HTML/ImageBitmap.cpp and
//! Libraries/LibWeb/HTML/MessagePort.cpp declare. A cell is the cell of an object of the interface that the name of
//! the function says. Libraries/LibWeb/HTML/BroadcastChannel.cpp hands the BroadcastChannel objects the messages that
//! other processes post, and Libraries/LibWeb/HTML/Window.cpp runs the window post message steps.
//! Libraries/LibWeb/HTML/TextTrackCue.cpp reads and updates the TextTrackCue objects that its text tracks hold.

use core::ffi::{c_int, c_void};
use core::ptr::NonNull;

use ak::Utf16String;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Gc, MarkedVec, Object, RawNativeFunctionResult, Realm, Value, Vm};
use liburl_rust::FfiOrigin;

use super::broadcast_channel::{BROADCAST_CHANNEL_MESSAGE_CLASS, BroadcastChannel, BroadcastChannelMessage};
use super::event_source::EventSource;
use super::image_bitmap::ImageBitmap;
use super::image_bitmap_rendering_context::ImageBitmapRenderingContext;
use super::image_data::ImageData;
use super::message_event::{MessageEvent, MessageEventInit, MessageEventSource};
use super::message_port::MessagePort;
use super::storage::{Storage, StorageType};
use super::storage_event::{StorageEvent, StorageEventInit};
use super::text_track_cue::AnyTextTrackCue;
use super::window::{PostedMessage, queue_posted_message, window_post_message_steps};
use super::window_or_worker_global_scope::structured_clone;
use crate::bindings::CallContext;
use crate::bindings::conversions::UsvString;
use crate::bindings::cpp_cell::{CppCell, LentCppCell, cpp_class};
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::event_target::CppEventTarget;
use crate::bindings::interface::{InterfaceCell, allocate, cell_from_abi, cell_into_abi, wrap};
use crate::bindings::lent_string::{LentUtf16FlyString, LentUtf16String};
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter, Transferable};
use crate::canvas_2d::canvas::Canvas;
use crate::dom::event::EventInit;
use crate::fetch::infrastructure::http::response_view::CppResponseView;
use crate::html::port_channel::give_up_on_parked_end;
use crate::html::structured_serialize::cpp::SerializedTransferRecord;
use crate::html::structured_serialize::message_record::MessageRecord;
use crate::html::structured_serialize::transfer_data_holder::{TransferDataHolder, TransferPayload};
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::storage_api::storage_area_mirror::StorageEndpointType;
use crate::storage_api::storage_bottle::{obtain_a_local_storage_bottle_map, obtain_a_session_storage_bottle_map};
use crate::storage_api::storage_key::StorageKey;

/// # Safety
///
/// `objects` must point to `count` live objects.
unsafe fn objects_from_abi(vm: &Vm, objects: *const *mut c_void, count: usize) -> MarkedVec<'_, Gc<Object>> {
    let list = MarkedVec::with_capacity(vm, count);
    // SAFETY: The caller passes `count` live objects.
    for &object in unsafe { ak::slice_from_raw_parts(objects, count) } {
        list.push(unsafe { Gc::from_non_null(NonNull::new(object.cast::<Object>()).expect("C++ passes objects")) });
    }
    list
}

/// Creates an ImageData of the size, with transparent black pixels and the sRGB color space, in the realm, and stores
/// its cell, or throws what initializing an ImageData throws.
#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_create(
    vm: &Vm,
    realm: Gc<Realm>,
    width: u32,
    height: u32,
    out_image_data: &mut *mut c_void,
) -> RawNativeFunctionResult {
    let mut cx = CallContext::new(vm, realm);
    let result = ImageData::create(&mut cx, width, height, None, None).map(|image_data| {
        *out_image_data = cell_into_abi(image_data);
        Value::UNDEFINED
    });
    RawNativeFunctionResult::from_completion(result.map_err(|exception| exception.throw(&cx)))
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_width(image_data: &InterfaceCell) -> u32 {
    image_data.object::<ImageData>().size().0
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_height(image_data: &InterfaceCell) -> u32 {
    image_data.object::<ImageData>().size().1
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_is_detached(image_data: &InterfaceCell) -> bool {
    image_data.object::<ImageData>().is_detached()
}

/// The colorSpace of the ImageData, as the underlying value of a Web::Bindings::PredefinedColorSpace.
#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_color_space(image_data: &InterfaceCell) -> u8 {
    image_data.object::<ImageData>().predefined_color_space() as u8
}

/// The first byte of the pixels of the ImageData, or null if its buffer is detached or its data has no room for them.
/// The pixels are valid until script runs.
#[unsafe(no_mangle)]
pub extern "C" fn web_image_data_pixels(image_data: &InterfaceCell) -> *mut u8 {
    let image_data = image_data.object::<ImageData>();
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

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_width(image_bitmap: &InterfaceCell) -> u32 {
    image_bitmap.object::<ImageBitmap>().width_of_bitmap()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_height(image_bitmap: &InterfaceCell) -> u32 {
    image_bitmap.object::<ImageBitmap>().height_of_bitmap()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_is_detached(image_bitmap: &InterfaceCell) -> bool {
    image_bitmap.object::<ImageBitmap>().is_detached()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_close(image_bitmap: &InterfaceCell) {
    image_bitmap.object::<ImageBitmap>().close_bitmap();
}

/// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_origin_clean(image_bitmap: &InterfaceCell) -> bool {
    image_bitmap.object::<ImageBitmap>().origin_clean()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_set_origin_clean(image_bitmap: &InterfaceCell, origin_clean: bool) {
    image_bitmap.object::<ImageBitmap>().set_origin_clean(origin_clean);
}

/// The Gfx::DecodedImageFrame of the bitmap data of the ImageBitmap, or null if it has none. The frame is valid until
/// the bitmap data is unset, by close() or a transfer.
#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_frame(image_bitmap: &InterfaceCell) -> *const c_void {
    image_bitmap.object::<ImageBitmap>().raw_frame()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_create(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(MessagePort::create(vm, realm))
}

/// Entangles the two ports, https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_entangle(vm: &Vm, port: &InterfaceCell, remote_port: &InterfaceCell) {
    let (port, remote_port) = (port.object::<MessagePort>(), remote_port.object::<MessagePort>());
    MessagePort::entangle(vm, port, remote_port);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_entangled_port(port: &InterfaceCell) -> *mut c_void {
    let port = port.object::<MessagePort>();
    port.entangled_port().map_or(core::ptr::null_mut(), cell_into_abi)
}

/// Sets the port's message event target, https://html.spec.whatwg.org/multipage/web-messaging.html#message-event-target
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_set_message_event_target(
    port: &InterfaceCell,
    event_target: &LentCppCell<cpp_class::EventTarget>,
) {
    let (port, event_target) = (port.object::<MessagePort>(), CppEventTarget::from_lent(event_target));
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
    vm: &Vm,
    realm: Gc<Realm>,
    port: &InterfaceCell,
    message: Value,
    transfer: *const *mut c_void,
    count: usize,
) -> RawNativeFunctionResult {
    let mut cx = CallContext::new(vm, realm);
    let port = port.object::<MessagePort>();
    // SAFETY: The caller passes `count` live objects.
    let transfer_list = unsafe { objects_from_abi(vm, transfer, count) };
    let result = MessagePort::post_message(&port, &mut cx, message, transfer_list);
    RawNativeFunctionResult::from_completion(
        result
            .map(|()| Value::UNDEFINED)
            .map_err(|exception| exception.throw(&cx)),
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_enable(vm: &Vm, port: &InterfaceCell) {
    let port = port.object::<MessagePort>();
    MessagePort::enable(port, vm);
}

/// The close() method steps of the port, https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-close
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_close(port: &InterfaceCell) {
    port.object::<MessagePort>().close_steps();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_discard_pending_messages(port: &InterfaceCell) {
    port.object::<MessagePort>().discard_pending_messages();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_pending_outgoing_message_count(port: &InterfaceCell) -> usize {
    port.object::<MessagePort>().pending_outgoing_message_count()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_fail_next_transfer_for_testing(port: &InterfaceCell) {
    port.object::<MessagePort>().fail_next_transfer_for_testing();
}

/// The transfer steps of the MessagePort of a new channel to a worker, which C++ runs outside
/// StructuredSerializeWithTransfer, into a data holder that holds no type, whose carrier it hands over. The data holder
/// goes through the browser process, and the token in `out_parked_end_token` is for settling what became of it.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_transfer_steps(
    vm: &Vm,
    realm: Gc<Realm>,
    port: &InterfaceCell,
    may_leave_process: bool,
    out_data_holder: &mut *mut c_void,
    out_parked_end_token: &mut u64,
) -> RawNativeFunctionResult {
    let port = port.object::<MessagePort>();
    let mut cx = CallContext::new(vm, realm);
    let data_holder = TransferDataHolder::new();
    let result = MessagePort::transfer_steps(&port, &mut cx, &mut TransferDataWriter::new(&data_holder, realm, None));
    if result.is_ok() {
        let mut payload = data_holder.finish();
        *out_parked_end_token = payload.park_channel_end(may_leave_process);
        *out_data_holder = payload.into_cpp();
    }
    RawNativeFunctionResult::from_completion(
        result
            .map(|()| Value::UNDEFINED)
            .map_err(|exception| exception.throw(&cx)),
    )
}

/// Tells that nobody in this process is going to claim the channel end of the token, unless somebody did.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_give_up_on_parked_end(token: u64) {
    give_up_on_parked_end(token);
}

/// How the port reaches the port it is entangled with, as an ASCII string that lives forever, for tests.
///
/// # Safety
///
/// `port` must be a live cell of a MessagePort, and `out_kind` must be valid for a write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_entanglement_kind(
    port: &InterfaceCell,
    out_kind: *mut ak::Span<'static, u8>,
) {
    let kind = port.object::<MessagePort>().entanglement_kind();
    // SAFETY: The caller passes a pointer that is valid for a write.
    unsafe { out_kind.write(ak::Span::new(kind.as_bytes())) };
}

/// Whether the transport of the port closed, for tests.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_transport_is_closed_for_testing(port: &InterfaceCell) -> bool {
    port.object::<MessagePort>().transport_is_closed_for_testing()
}

/// The transfer-receiving steps of a MessagePort that C++ runs outside StructuredDeserializeWithTransfer, which return
/// the wrapper of the new MessagePort.
///
/// # Safety
///
/// `vm` must be the live VM, `realm` a live realm, and `data_holder` an HTML::TransferDataEncoder that C++ hands over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_message_port_transfer_receiving_steps(
    vm: &Vm,
    realm: Gc<Realm>,
    data_holder: *mut c_void,
) -> RawNativeFunctionResult {
    // SAFETY: The caller hands over the data holder.
    let mut payload = unsafe { TransferPayload::from_cpp(data_holder) };
    let mut cx = CallContext::new(vm, realm);
    let mut data_holder = TransferDataReader::new(&mut payload, realm);
    let result = MessagePort::transfer_receiving_steps(&mut cx, &mut data_holder);
    RawNativeFunctionResult::from_completion(
        result
            .map(|port| Value::from_object(wrap(&cx, port)))
            .map_err(|exception| exception.throw(&cx)),
    )
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_message_port_disentangle_ports_of_realm(vm: &Vm, realm: Gc<Realm>) {
    MessagePort::disentangle_ports_of_realm(vm, realm);
}

/// Releases what the thread's agent keeps outside its heap, for an agent that goes away.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_shut_down_agent(vm: &Vm) {
    MessagePort::shut_down_ports_of_agent(vm);
    crate::html::broadcast_channel::forget_broadcast_channels_of_agent();
    crate::html::event_source::forget_event_sources_of_agent();
    crate::web_gl::context_loss::forget_contexts_of_agent();
    crate::web_assembly::Agent::shut_down();
    crate::indexed_db::internal::storage::shut_down_storage();
    crate::permissions_api::permission_store::forget_statuses_of_this_agent();
}

/// Steps 6 to 9 of BroadcastChannel's postMessage(), for a message that another process posted, whose channel name
/// C++ hands over.
///
/// # Safety
///
/// `vm` must be the live VM, the origins and the bytes lent, `channel_name` an owned AK::Utf16String, and
/// `shared_store_lifeline` a descriptor that is open for the call, or negative.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_broadcast_channel_deliver_message_locally(
    vm: &Vm,
    storage_key_origin: *const FfiOrigin,
    channel_name: Utf16String,
    source_origin: *const FfiOrigin,
    serialized_message: ak::Span<'_, u8>,
    shared_store_lifeline: c_int,
) {
    // SAFETY: The caller lends the origins.
    let (storage_key, source_origin) = unsafe {
        (
            StorageKey {
                origin: (*storage_key_origin).to_origin(),
            },
            (*source_origin).to_origin(),
        )
    };
    let message = BROADCAST_CHANNEL_MESSAGE_CLASS.allocate(vm, |defer_gc| BroadcastChannelMessage {
        storage_key,
        channel_name,
        source_origin,
        // SAFETY: The caller keeps the descriptor open for the call.
        serialized: unsafe {
            MessageRecord::received(defer_gc, serialized_message.as_slice(), &[], &[], shared_store_lifeline)
        },
    });
    BroadcastChannel::deliver_message_locally(vm, message, None);
}

/// A new Storage object of the type, 0 for "local" and 1 for "session", for the Window of the realm, whose map is a new
/// storage proxy map over the bottle for the realm's settings object: steps 2 to 4 of the localStorage and
/// sessionStorage getters. Null when obtaining the bottle map fails.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_html_create_storage(vm: &Vm, realm: Gc<Realm>, storage_type: u8) -> *mut c_void {
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
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_html_storage_connect(storage: &InterfaceCell) {
    storage.implementation::<Storage>().map().connect();
}

/// Calls `on_item` with each key of the map of the Storage object and its value, raw AK::Utf16Strings that Rust lends
/// for the call.
///
/// # Safety
///
/// `storage` must be the live cell of a Storage object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_storage_for_each_item(
    storage: &InterfaceCell,
    ctx: *mut c_void,
    on_item: unsafe extern "C" fn(*mut c_void, usize, usize),
) {
    let map = storage.implementation::<Storage>().map();
    for key in map.keys() {
        if let Some(value) = map.get(&key) {
            // SAFETY: The strings outlive the call.
            unsafe { on_item(ctx, key.raw_identity(), value.raw_identity()) };
        }
    }
}

/// The Event of a new StorageEvent in the realm whose type is the empty string and whose attributes are their
/// defaults, for createEvent().
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_html_create_storage_event(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
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

/// The Event of a new trusted MessageEvent in the realm, for C++ to fire. Its origin is the origin if there is one,
/// and the empty string otherwise.
///
/// # Safety
///
/// The VM, the realm, the data and each port must be live, and so must the source if it is not null. The type, the
/// port array and the origin are lent for the call.
#[allow(clippy::too_many_arguments, reason = "C++ passes each member of the dictionary")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_html_create_message_event(
    vm: &Vm,
    realm: Gc<Realm>,
    r#type: LentUtf16FlyString<'_>,
    data: Value,
    ports: *const *mut c_void,
    count: usize,
    source: Option<&InterfaceCell>,
    origin: *const FfiOrigin,
) -> *mut c_void {
    let mut event_init = MessageEventInit::new(vm);
    event_init.data = data;
    // SAFETY: The caller lends the ports for the call.
    for &port in unsafe { ak::slice_from_raw_parts(ports, count) } {
        // SAFETY: Each port is a live MessagePort.
        event_init.ports.push(unsafe { cell_from_abi::<MessagePort>(port) });
    }
    if let Some(source) = source {
        event_init.source = Some(MessageEventSource::MessagePort(source.object::<MessagePort>()));
    }
    // SAFETY: An origin that is not null is lent for the call.
    let origin = (!origin.is_null()).then(|| unsafe { (*origin).to_origin() });
    let r#type: Utf16String = r#type.to_utf16_fly_string().into();
    let event = MessageEvent::create(vm, realm, &r#type, event_init, origin);
    event.event().as_ptr()
}

/// The Event of a new MessageEvent in the realm whose type is the empty string, for createEvent().
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_html_create_message_event_for_create_event(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    MessageEvent::create_for_create_event(vm, realm).event().as_ptr()
}

/// # Safety
///
/// The VM, the realm, the target and the transferred objects must be live, and the target must be a Window if remote
/// is false and a RemoteNavigable otherwise. The target origin string is handed over, and the transfer list is lent.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_window_post_message(
    vm: &Vm,
    realm: Gc<Realm>,
    target: &LentCppCell<cpp_class::Cell>,
    remote: bool,
    message: Value,
    target_origin: Utf16String,
    transfer: *const *mut c_void,
    count: usize,
) -> RawNativeFunctionResult {
    let cx = CallContext::new(vm, realm);
    // SAFETY: The caller lends the live target, message and transferred objects, and hands over the string.
    let (target, transfer) = unsafe { (CppCell::from_lent(target), objects_from_abi(vm, transfer, count)) };
    let result = window_post_message_steps(&cx, target, remote, message, &target_origin, &transfer.values());
    RawNativeFunctionResult::from_completion(result.map(|()| Value::UNDEFINED).map_err(|error| error.throw(&cx)))
}

/// # Safety
///
/// The VM, the Window and the source, if any, must be live, and the record and the origins are lent for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_window_queue_posted_message(
    vm: &Vm,
    target_window: &LentCppCell<cpp_class::Window>,
    record: *mut c_void,
    target_origin: *const FfiOrigin,
    origin: *const FfiOrigin,
    source: *mut c_void,
) {
    // SAFETY: The caller passes the live VM, and lends the Window, the source, the record and the origins.
    unsafe {
        let posted_message = PostedMessage {
            target_window: CppCell::from_lent(target_window),
            serialize_with_transfer_result: TransferRecord::from_cpp(vm, SerializedTransferRecord::from_raw(record)),
            target_origin: target_origin.as_ref().map(|origin| origin.to_origin()),
            origin: (*origin).to_origin(),
            source: NonNull::new(source).map(|source| CppCell::from_raw(source.as_ptr())),
        };
        queue_posted_message(vm, posted_message);
    }
}

/// # Safety
///
/// The VM and the realm must be live, and `transfer` must point to `count` live objects.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_structured_clone(
    vm: &Vm,
    realm: Gc<Realm>,
    value: Value,
    transfer: *const *mut c_void,
    count: usize,
) -> RawNativeFunctionResult {
    let cx = CallContext::new(vm, realm);
    // SAFETY: The caller passes a live value and `count` live objects.
    let transfer = unsafe { objects_from_abi(vm, transfer, count) };
    let result = structured_clone(&cx, value, &transfer.values());
    RawNativeFunctionResult::from_completion(result.map_err(|exception| exception.throw(&cx)))
}

/// Creates the ImageBitmapRenderingContext of the HTMLCanvasElement whose cell is `canvas` and whose
/// Web::HTML::CanvasHost is `canvas_host`, and returns its cell.
///
/// # Safety
///
/// `canvas` must be the live cell of the canvas, and `canvas_host` the same object as a Web::HTML::CanvasHost.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_image_bitmap_rendering_context_create(
    vm: &Vm,
    canvas: *mut c_void,
    canvas_host: *mut c_void,
    alpha: bool,
) -> *mut c_void {
    let canvas = Canvas {
        cell: NonNull::new(canvas).expect("C++ passes the canvas"),
        host: NonNull::new(canvas_host).expect("C++ passes the canvas"),
        is_offscreen: false,
    };
    cell_into_abi(allocate(vm, |defer_gc| {
        ImageBitmapRenderingContext::new(defer_gc, canvas, alpha)
    }))
}

/// The Gfx::DecodedImageFrame of the output bitmap of the context, which stays valid until the output bitmap changes,
/// or null for none.
#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_rendering_context_frame(context: &InterfaceCell) -> *const c_void {
    context.object::<ImageBitmapRenderingContext>().raw_frame()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_rendering_context_is_blank(context: &InterfaceCell) -> bool {
    context.object::<ImageBitmapRenderingContext>().is_blank()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_rendering_context_origin_clean(context: &InterfaceCell) -> bool {
    context.object::<ImageBitmapRenderingContext>().origin_clean()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_image_bitmap_rendering_context_alpha(context: &InterfaceCell) -> bool {
    context.object::<ImageBitmapRenderingContext>().alpha()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_event_target(cue: &InterfaceCell) -> *mut c_void {
    AnyTextTrackCue::from_abi(cue).event_target().as_ptr()
}

/// The cell of the TextTrack whose list of cues the cue is in, or null.
#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_track(cue: &InterfaceCell) -> *mut c_void {
    let cue = AnyTextTrackCue::from_abi(cue);
    cue.track_cell().map_or(core::ptr::null_mut(), |track| track.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_set_track(cue: &InterfaceCell, track: Option<&LentCppCell<cpp_class::TextTrack>>) {
    let cue = AnyTextTrackCue::from_abi(cue);
    cue.set_track_cell(track.map(CppCell::from_lent));
}

/// The identifier of the cue, whose reference the caller adopts.
#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_id(cue: &InterfaceCell) -> usize {
    AnyTextTrackCue::from_abi(cue).identifier().into_raw()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_set_id(cue: &InterfaceCell, id: LentUtf16String<'_>) {
    let (cue, id) = (AnyTextTrackCue::from_abi(cue), id.to_utf16_string());
    cue.set_identifier(id);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_start_time(cue: &InterfaceCell) -> f64 {
    AnyTextTrackCue::from_abi(cue).start_time_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_end_time(cue: &InterfaceCell) -> f64 {
    AnyTextTrackCue::from_abi(cue).end_time_value()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_pause_on_exit(cue: &InterfaceCell) -> bool {
    AnyTextTrackCue::from_abi(cue).pause_on_exit_flag()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_is_active(cue: &InterfaceCell) -> bool {
    AnyTextTrackCue::from_abi(cue).is_active()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_set_active(cue: &InterfaceCell, active: bool) {
    AnyTextTrackCue::from_abi(cue).set_active(active);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_order_added(cue: &InterfaceCell) -> u64 {
    AnyTextTrackCue::from_abi(cue).order_added()
}

#[unsafe(no_mangle)]
pub extern "C" fn web_text_track_cue_set_order_added(cue: &InterfaceCell, order_added: u64) {
    AnyTextTrackCue::from_abi(cue).set_order_added(order_added);
}

/// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_event_source_process_response(
    vm: &Vm,
    event_source: &InterfaceCell,
    response: &LentCppCell<cpp_class::ResponseView>,
) {
    let (event_source, response) = (
        event_source.object::<EventSource>(),
        CppResponseView::from_lent(response),
    );
    EventSource::process_response(event_source, vm, response);
}

/// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_event_source_process_response_end_of_body(
    vm: &Vm,
    event_source: &InterfaceCell,
    response: &LentCppCell<cpp_class::ResponseView>,
) {
    let (event_source, response) = (
        event_source.object::<EventSource>(),
        CppResponseView::from_lent(response),
    );
    EventSource::process_event_source_end_of_body(event_source, vm, response);
}

/// https://html.spec.whatwg.org/multipage/server-sent-events.html#dom-eventsource
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_event_source_process_body_chunk(
    vm: &Vm,
    event_source: &InterfaceCell,
    bytes: ak::Span<'_, u8>,
) {
    let event_source = event_source.object::<EventSource>();
    EventSource::process_body_chunk(event_source, vm, bytes.as_slice());
}

/// https://html.spec.whatwg.org/multipage/server-sent-events.html#concept-eventsource-forcibly-close
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_event_source_forcibly_close_all(vm: &Vm, realm: Gc<Realm>) {
    EventSource::forcibly_close_all(vm, realm);
}
