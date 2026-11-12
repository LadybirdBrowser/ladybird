/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Clipboard, https://w3c.github.io/clipboard-apis/#clipboard

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    EmbedderCell, EmbedderClass, EmbedderValue, Gc, GcRefCell, MarkedVec, PrimitiveString, Promise, PromiseState,
    Realm, Trace, Value, Vm, is_host_instance_of,
};
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use crate::bindings::conversions::rooted_sequence_to_js;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::global_object::{CppWindow, window_from_global_object};
use crate::bindings::interface::{
    Interface, Ref, Wrappable, allocate, implementation_if, with_temporary_execution_context,
    with_temporary_execution_context_and_callbacks, wrap,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, deferred_invoke, promise_realm, queue_a_global_task, react_to_promise,
    reject_promise_with_exception, resolve_promise, spin_until_promise_is_settled,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::clipboard::clipboard_item::{ClipboardItem, Representation};
use crate::file_api::blob::{AnyBlob, Blob};
use crate::file_api::file::File;

#[repr(C)]
struct SystemClipboardRepresentationAbi<'a> {
    name: ak::Span<'a, u8>,
    data: ak::Span<'a, u8>,
}

unsafe extern "C" {
    /// Hands over the data that the Window's paste is pasting, if it is pasting one.
    fn web_clipboard_system_clipboard_data_being_pasted(
        window: *mut c_void,
        data: *mut c_void,
        on_item: unsafe extern "C" fn(*mut c_void),
        on_representation: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, u8>),
    ) -> bool;

    /// Calls the steps with the VM, the request and the system clipboard data, a Vector<SystemClipboardItem>, once the UI
    /// process hands it over.
    fn web_clipboard_request_system_clipboard_data(
        window: *mut c_void,
        request: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_void),
    );

    fn web_clipboard_for_each_system_clipboard_item(
        system_clipboard_data: *const c_void,
        data: *mut c_void,
        on_item: unsafe extern "C" fn(*mut c_void),
        on_representation: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, ak::Span<'_, u8>),
    );

    fn web_clipboard_insert_clipboard_item(
        window: *mut c_void,
        representations: ak::Span<'_, SystemClipboardRepresentationAbi<'_>>,
        presentation_style: ak::Span<'_, u8>,
    );
}

// https://w3c.github.io/clipboard-apis/#system-clipboard-representation
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SystemClipboardRepresentation {
    #[gc(untraced)]
    pub(crate) name: String,
    #[gc(untraced)]
    pub(crate) data: Vec<u8>,
}

// https://w3c.github.io/clipboard-apis/#system-clipboard-item
#[derive(Clone, Default, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SystemClipboardItem {
    pub(crate) system_clipboard_representations: Vec<SystemClipboardRepresentation>,
}

unsafe extern "C" fn on_system_clipboard_item(data: *mut c_void) {
    // SAFETY: The data is the list of the caller.
    unsafe { &mut *data.cast::<Vec<SystemClipboardItem>>() }.push(SystemClipboardItem::default());
}

unsafe extern "C" fn on_system_clipboard_representation(
    data: *mut c_void,
    name: ak::Span<'_, u8>,
    representation_data: ak::Span<'_, u8>,
) {
    // SAFETY: The data is the list of the caller, and LibWeb lends the name and the data for the call.
    let data = unsafe { &mut *data.cast::<Vec<SystemClipboardItem>>() };
    data.last_mut()
        .expect("LibWeb hands over an item before its representations")
        .system_clipboard_representations
        .push(SystemClipboardRepresentation {
            name: String::from_utf8_lossy(name.as_slice()).into_owned(),
            data: representation_data.as_slice().to_vec(),
        });
}

fn system_clipboard_data_being_pasted(window: &CppWindow) -> Option<Vec<SystemClipboardItem>> {
    let mut data = Vec::new();
    // SAFETY: The Window is live, and the list outlives the call.
    let is_pasting = unsafe {
        web_clipboard_system_clipboard_data_being_pasted(
            window.as_ptr(),
            (&raw mut data).cast(),
            on_system_clipboard_item,
            on_system_clipboard_representation,
        )
    };
    is_pasting.then_some(data)
}

pub(crate) fn string_to_code_points(string: &Utf16String) -> Vec<char> {
    char::decode_utf16(string.to_utf16().iter().copied())
        .map(|code_point| code_point.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect()
}

pub(crate) fn clipboard_item_data_blob(value: Value) -> Option<AnyBlob> {
    if !value.is_object() || !is_host_instance_of(&value.as_object(), <Blob as Interface>::wrapper_class()) {
        return None;
    }
    if let Some(blob) = implementation_if::<Blob>(value.as_object()) {
        return Some(AnyBlob::Blob(blob));
    }
    implementation_if::<File>(value.as_object()).map(AnyBlob::File)
}

fn not_allowed_error(message: &str) -> Exception {
    Exception::Dom("NotAllowedError", Utf16String::from_utf8(message))
}

// https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
pub(crate) fn os_specific_well_known_format(mime_type: &MimeType) -> String {
    // 1. Let wellKnownFormat be an empty string.
    let mut well_known_format = String::new();

    let essence = mime_type.essence();
    let essence = format!("{}/{}", essence.type_, essence.subtype);

    // 2. If mimeType’s essence is "text/plain", then
    if essence == "text/plain" {
        // On Windows, follow the convention described below:
        //     Assign CF_UNICODETEXT to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypeString to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/plain" to wellKnownFormat.
        well_known_format = essence;
    }
    // 3. Else, if mimeType’s essence is "text/html", then
    else if essence == "text/html" {
        // On Windows, follow the convention described below:
        //     Assign CF_HTML to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSHTMLPboardType to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/html" to wellKnownFormat.
        well_known_format = essence;
    }
    // 4. Else, if mimeType’s essence is "image/png", then
    else if essence == "image/png" {
        // On Windows, follow the convention described below:
        //     Assign "PNG" to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypePNG to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "image/png" to wellKnownFormat.
        well_known_format = essence;
    }

    // 5. Return wellKnownFormat.
    well_known_format
}

fn os_specific_well_known_format_of_string(mime_type_string: &str) -> String {
    // NOTE: Here we always takes the Linux case, and defer to the browser process to handle OS specific implementations.
    let code_points: Vec<char> = mime_type_string.chars().collect();
    let Some(mime_type) = MimeType::parse(&code_points) else {
        return String::new();
    };

    os_specific_well_known_format(&mime_type)
}

// https://w3c.github.io/clipboard-apis/#write-blobs-and-option-to-the-clipboard
fn write_blobs_and_option_to_clipboard(window: &CppWindow, items: &[AnyBlob], presentation_style: &str) {
    let mut representations: Vec<SystemClipboardRepresentation> = Vec::new();

    // FIXME: 1. Let webCustomFormats be a sequence<Blob>.

    // 2. For each item in items:
    for item in items {
        // 1. Let formatString be the result of running os specific well-known format given item’s type.
        let format_string =
            os_specific_well_known_format_of_string(&String::from_utf16_lossy(&item.type_string().to_utf16()));

        // 2. If formatString is empty then follow the below steps:
        if format_string.is_empty() {
            // FIXME: 1. Let webCustomFormatString be the item’s type.
            // FIXME: 2. Let webCustomFormat be an empty type.
            // FIXME: 3. If webCustomFormatString starts with `"web "` prefix, then remove the `"web "` prefix and store the
            // FIXME:    remaining string in webMimeTypeString.
            // FIXME: 4. Let webMimeType be the result of parsing a MIME type given webMimeTypeString.
            // FIXME: 5. If webMimeType is failure, then abort all steps.
            // FIXME: 6. Let webCustomFormat’s type's essence equal to webMimeType.
            // FIXME: 7. Set item’s type to webCustomFormat.
            // FIXME: 8. Append webCustomFormat to webCustomFormats.
        }

        // 3. Let payload be the result of UTF-8 decoding item’s underlying byte sequence.
        // FIXME: Spec issue: UTF-8 decoding here does not make sense. This would make a simple clipboard.write ->
        //        clipboard.read sequence for PNG data corrupt.
        //        https://github.com/w3c/clipboard-apis/issues/217
        let payload = item.raw_bytes().to_vec();

        // 4. Insert payload and presentationStyle into the system clipboard using formatString as the native clipboard format.
        representations.push(SystemClipboardRepresentation {
            name: format_string,
            data: payload,
        });
    }

    let representations: Vec<SystemClipboardRepresentationAbi<'_>> = representations
        .iter()
        .map(|representation| SystemClipboardRepresentationAbi {
            name: ak::Span::new(representation.name.as_bytes()),
            data: ak::Span::new(&representation.data),
        })
        .collect();
    // SAFETY: The Window is live, and the representations and the presentation style are lent for the call.
    unsafe {
        web_clipboard_insert_clipboard_item(
            window.as_ptr(),
            ak::Span::new(&representations),
            ak::Span::new(presentation_style.as_bytes()),
        );
    }

    // FIXME: 3. Write web custom formats given webCustomFormats.
}

// AD-HOC: Besides true and false, checking the clipboard read permission can allow reading only data that the reader's
//         own origin wrote to the clipboard.
#[derive(Clone, Copy, Eq, PartialEq)]
enum ClipboardReadPermission {
    Denied,
    Granted,
    GrantedForOwnOriginData,
}

fn relevant_window(realm: Gc<Realm>) -> CppWindow {
    window_from_global_object(realm.global_object()).expect("Clipboard is exposed only in Windows")
}

// https://w3c.github.io/clipboard-apis/#h-clipboard-read-permission
fn check_clipboard_read_permission(realm: Gc<Realm>, is_running_from_paste: bool) -> ClipboardReadPermission {
    // NOTE: The clipboard permission is undergoing a refactor because the clipboard-read permission was removed from
    //       the Permissions spec. So this partially implements the proposed update:
    //       https://pr-preview.s3.amazonaws.com/w3c/clipboard-apis/pull/164.html#read-permission

    let window = relevant_window(realm);

    // NB: "This API must not be available unless the script is executing in the context of a document that has focus."
    //     https://w3c.github.io/clipboard-apis/#privacy-async
    if !window.associated_document().has_focus() {
        return ClipboardReadPermission::Denied;
    }

    // 1. Let hasGesture be true if the relevant global object of this has transient activation, false otherwise.
    let has_gesture = window.has_transient_activation();

    // 2. If hasGesture then,
    if has_gesture {
        // 1. Return true if the current script is running as a result of user interaction with a "Paste" element
        //    created by the user agent or operating system.
        if is_running_from_paste {
            return ClipboardReadPermission::Granted;
        }

        // AD-HOC: Reading back data that the reader's own origin wrote to the clipboard tells it nothing new, so allow
        //         that without a "Paste" element.
        return ClipboardReadPermission::GrantedForOwnOriginData;
    }

    // 3. Otherwise, return false.
    ClipboardReadPermission::Denied
}

// https://w3c.github.io/clipboard-apis/#check-clipboard-write-permission
fn check_clipboard_write_permission(realm: Gc<Realm>) -> bool {
    // NOTE: The clipboard permission is undergoing a refactor because the clipboard-write permission was removed from
    //       the Permissions spec. So this partially implements the proposed update:
    //       https://pr-preview.s3.amazonaws.com/w3c/clipboard-apis/pull/164.html#write-permission

    // 1. Let hasGesture be true if the relevant global object of this has transient activation, false otherwise.
    let has_gesture = relevant_window(realm).has_transient_activation();

    // 2. If hasGesture then,
    if has_gesture {
        // FIXME: 1. Return true if the current script is running as a result of user interaction with a "cut" or "copy"
        //           element created by the user agent or operating system.
        return true;
    }

    // 3. Otherwise, return false.
    false
}

fn queue_a_permission_denial(vm: &Vm, realm: Gc<Realm>, promise: WebIdlPromise, message: &'static str) {
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::Permissions,
        (promise, message),
        |vm, (promise, message)| {
            with_temporary_execution_context(promise_realm(promise), || {
                reject_promise_with_exception(vm, promise, not_allowed_error(message));
            });
        },
    );
}

const READING_IS_NOT_ALLOWED: &str = "Clipboard reading is only allowed while the user pastes";
const WRITING_IS_NOT_ALLOWED: &str = "Clipboard writing is only allowed through user activation";

/// The dictionary ClipboardUnsanitizedFormats of Clipboard/Clipboard.idl.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ClipboardUnsanitizedFormats {
    pub(crate) unsanitized: Option<Vec<Utf16String>>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ReadRequest {
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    formats: ClipboardUnsanitizedFormats,
}

static READ_REQUEST_CLASS: EmbedderClass<ReadRequest> = EmbedderClass::new("ClipboardReadRequest");

impl EmbedderValue for ReadRequest {
    const CLASS: &'static EmbedderClass<Self> = &READ_REQUEST_CLASS;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ReadTextRequest {
    realm: Gc<Realm>,
    promise: WebIdlPromise,
}

static READ_TEXT_REQUEST_CLASS: EmbedderClass<ReadTextRequest> = EmbedderClass::new("ClipboardReadTextRequest");

impl EmbedderValue for ReadTextRequest {
    const CLASS: &'static EmbedderClass<Self> = &READ_TEXT_REQUEST_CLASS;
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
struct ReadItem {
    item: Ref<ClipboardItem>,
    originating: Option<SystemClipboardItem>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ReadItems {
    request: Gc<EmbedderCell<ReadRequest>>,
    snapshot_change_count: usize,
    items: GcRefCell<Vec<ReadItem>>,
}

static READ_ITEMS_CLASS: EmbedderClass<ReadItems> = EmbedderClass::new("ClipboardReadItems");

impl EmbedderValue for ReadItems {
    const CLASS: &'static EmbedderClass<Self> = &READ_ITEMS_CLASS;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct WriteData {
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    data: Vec<Ref<ClipboardItem>>,
}

static WRITE_DATA_CLASS: EmbedderClass<WriteData> = EmbedderClass::new("ClipboardWriteData");

impl EmbedderValue for WriteData {
    const CLASS: &'static EmbedderClass<Self> = &WRITE_DATA_CLASS;
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct ItemList {
    realm: Gc<Realm>,
    promise: WebIdlPromise,
    blobs: GcRefCell<Vec<AnyBlob>>,
}

static ITEM_LIST_CLASS: EmbedderClass<ItemList> = EmbedderClass::new("ClipboardItemList");

impl EmbedderValue for ItemList {
    const CLASS: &'static EmbedderClass<Self> = &ITEM_LIST_CLASS;
}

// https://w3c.github.io/clipboard-apis/#clipboard
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Clipboard {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
}

impl Wrappable for Clipboard {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for Clipboard {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl Clipboard {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Clipboard> {
        let clipboard = allocate(vm, |_| Clipboard {
            event_target: CppEventTarget::new(),
            realm,
        });
        clipboard.event_target.set_owner(clipboard);
        clipboard
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboard-read
    pub(crate) fn read(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        formats: ClipboardUnsanitizedFormats,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 3. If formats is not empty, then:
        if let Some(unsanitized) = &formats.unsanitized {
            // 1. For each format in formats["unsanitized"]:
            for _format in unsanitized {
                // FIXME: 1. If format is not in optional unsanitized data types, then reject p with format "NotAllowedError"
                //           DOMException in realm.
            }
        }

        // NB: The steps below run in parallel, after the script that called this has returned. So take now the data that a
        //     paste of the system clipboard is pasting, which a script running as a result of that paste reads.
        let pasted_data = system_clipboard_data_being_pasted(&relevant_window(realm));

        // 4. Run the following steps in parallel:
        let request = READ_REQUEST_CLASS.allocate(vm, |_| ReadRequest {
            realm,
            promise,
            formats,
        });
        deferred_invoke(vm, (request, pasted_data), |vm, (request, pasted_data)| {
            // 1. Let r be the result of running check clipboard read permission.
            let result = check_clipboard_read_permission(request.realm, pasted_data.is_some());

            // 2. If r is false, then:
            if result == ClipboardReadPermission::Denied {
                // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
                //    "NotAllowedError" DOMException in realm.
                queue_a_permission_denial(vm, request.realm, request.promise, READING_IS_NOT_ALLOWED);

                // 2. Abort these steps.
                return;
            }

            // 3. Let data be the system clipboard data.
            // NB: A read running as a result of a paste of the system clipboard reads the data being pasted.
            if result == ClipboardReadPermission::Granted {
                read_steps_with_data(
                    vm,
                    request,
                    pasted_data.expect("a read granted as a result of a paste has its data"),
                );
                return;
            }

            // AD-HOC: For any other read, the UI process hands over the system clipboard data only if all of it is data that
            //         the reading document's origin wrote.
            request_system_clipboard_data(request.realm, request, read_with_requested_data);
        });
        Ok(())
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboard-readtext
    pub(crate) fn read_text(&self, cx: &mut CallContext, promise: WebIdlPromise) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = cx.realm();

        // NB: The steps below run in parallel, after the script that called this has returned. So take now the data that a
        //     paste of the system clipboard is pasting, which a script running as a result of that paste reads.
        let pasted_data = system_clipboard_data_being_pasted(&relevant_window(realm));

        // 3. Run the following steps in parallel:
        let request = READ_TEXT_REQUEST_CLASS.allocate(vm, |_| ReadTextRequest { realm, promise });
        deferred_invoke(vm, (request, pasted_data), |vm, (request, pasted_data)| {
            // 1. Let r be the result of running check clipboard read permission.
            let result = check_clipboard_read_permission(request.realm, pasted_data.is_some());

            // 2. If r is false, then:
            if result == ClipboardReadPermission::Denied {
                // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
                //    "NotAllowedError" DOMException in realm.
                queue_a_permission_denial(vm, request.realm, request.promise, READING_IS_NOT_ALLOWED);

                // 2. Abort these steps.
                return;
            }

            // 3. Let data be a copy of the system clipboard data.
            // NB: A read running as a result of a paste of the system clipboard reads the data being pasted.
            if result == ClipboardReadPermission::Granted {
                read_text_steps_with_data(
                    vm,
                    request,
                    pasted_data.expect("a read granted as a result of a paste has its data"),
                );
                return;
            }

            // AD-HOC: For any other read, the UI process hands over the system clipboard data only if all of it is data that
            //         the reading document's origin wrote.
            request_system_clipboard_data(request.realm, request, read_text_with_requested_data);
        });
        Ok(())
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboard-write
    pub(crate) fn write(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        data: MarkedVec<'_, Ref<ClipboardItem>>,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = cx.realm();
        let data = WRITE_DATA_CLASS.allocate(vm, |_| WriteData {
            realm,
            promise,
            data: data.to_vec(),
        });

        // 3. Run the following steps in parallel:
        deferred_invoke(vm, data, |vm, data| {
            // 1. Let r be the result of running check clipboard write permission.
            let result = check_clipboard_write_permission(data.realm);

            // 2. If r is false, then:
            if !result {
                // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
                //    "NotAllowedError" DOMException in realm.
                queue_a_permission_denial(vm, data.realm, data.promise, WRITING_IS_NOT_ALLOWED);

                // 2. Abort these steps.
                return;
            }

            // 3. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
            queue_a_global_task(
                vm,
                data.realm.global_object(),
                TaskSource::Clipboard,
                data,
                |vm, data| {
                    with_temporary_execution_context_and_callbacks(data.realm, || write_task(vm, data));
                },
            );
        });
        Ok(())
    }

    // https://w3c.github.io/clipboard-apis/#dom-clipboard-writetext
    pub(crate) fn write_text(
        &self,
        cx: &mut CallContext,
        promise: WebIdlPromise,
        data: Utf16String,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = cx.realm();

        // 3. Run the following steps in parallel:
        deferred_invoke(vm, (realm, promise, data), |vm, (realm, promise, data)| {
            // 1. Let r be the result of running check clipboard write permission.
            let result = check_clipboard_write_permission(realm);

            // 2. If r is false, then:
            if !result {
                // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
                //    "NotAllowedError" DOMException in realm.
                queue_a_permission_denial(vm, realm, promise, WRITING_IS_NOT_ALLOWED);

                // 2. Abort these steps.
                return;
            }

            // 3. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
            queue_a_global_task(
                vm,
                realm.global_object(),
                TaskSource::Clipboard,
                (realm, promise, data),
                |vm, (realm, promise, data)| {
                    // 1. Let itemList be an empty sequence<Blob>.
                    let item_list = MarkedVec::new(vm);

                    // 2. Let textBlob be a new Blob created with: type attribute set to "text/plain;charset=utf-8", and its
                    //    underlying byte sequence set to the UTF-8 encoding of data.
                    //    Note: On Windows replace `\n` characters with `\r\n` in data before creating textBlob.
                    let data_as_bytes = String::from_utf16_lossy(&data.to_utf16()).into_bytes();
                    let text_blob = allocate(vm, |_| {
                        Blob::new(data_as_bytes, Utf16String::from_utf8("text/plain;charset=utf-8"))
                    });

                    // 3. Add textBlob to itemList.
                    item_list.push(AnyBlob::Blob(text_blob));

                    // 4. Let option be set to "unspecified".
                    let option = "unspecified";

                    // 5. Write blobs and option to the clipboard with itemList and option.
                    write_blobs_and_option_to_clipboard(&relevant_window(realm), &item_list.values(), option);

                    // 6. Resolve p.
                    with_temporary_execution_context(realm, || {
                        resolve_promise(vm, promise, Value::UNDEFINED);
                    });
                },
            );
        });
        Ok(())
    }
}

fn request_system_clipboard_data<T: EmbedderValue>(
    realm: Gc<Realm>,
    request: Gc<EmbedderCell<T>>,
    steps: unsafe extern "C" fn(*mut c_void, *mut c_void, *const c_void),
) {
    let window = relevant_window(realm);
    // SAFETY: The Window and the request are live.
    unsafe { web_clipboard_request_system_clipboard_data(window.as_ptr(), request.as_ptr().cast(), steps) };
}

/// # Safety
///
/// The pointers must be the live VM, a live request of the type and a live Vector<SystemClipboardItem>.
unsafe fn requested_data_arguments<'a, T: EmbedderValue>(
    vm: *mut c_void,
    request: *mut c_void,
    system_clipboard_data: *const c_void,
) -> (&'a Vm, Gc<EmbedderCell<T>>, Vec<SystemClipboardItem>) {
    let mut data = Vec::new();
    // SAFETY: The caller passes live pointers, and the list outlives the call.
    unsafe {
        web_clipboard_for_each_system_clipboard_item(
            system_clipboard_data,
            (&raw mut data).cast(),
            on_system_clipboard_item,
            on_system_clipboard_representation,
        );
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new(request.cast()).expect("LibWeb passes the request")),
            data,
        )
    }
}

unsafe extern "C" fn read_with_requested_data(vm: *mut c_void, request: *mut c_void, data: *const c_void) {
    // SAFETY: LibWeb passes the VM, the request that read() made and the data.
    let (vm, request, data) = unsafe { requested_data_arguments::<ReadRequest>(vm, request, data) };
    if data.is_empty() {
        queue_a_permission_denial(vm, request.realm, request.promise, READING_IS_NOT_ALLOWED);
        return;
    }
    read_steps_with_data(vm, request, data);
}

unsafe extern "C" fn read_text_with_requested_data(vm: *mut c_void, request: *mut c_void, data: *const c_void) {
    // SAFETY: LibWeb passes the VM, the request that readText() made and the data.
    let (vm, request, data) = unsafe { requested_data_arguments::<ReadTextRequest>(vm, request, data) };
    if data.is_empty() {
        queue_a_permission_denial(vm, request.realm, request.promise, READING_IS_NOT_ALLOWED);
        return;
    }
    read_text_steps_with_data(vm, request, data);
}

// NB: The steps of read() after step 3 run once the system clipboard data is in hand.
fn read_steps_with_data(vm: &Vm, request: Gc<EmbedderCell<ReadRequest>>, data: Vec<SystemClipboardItem>) {
    let realm = request.realm;
    with_temporary_execution_context(realm, || {
        // 4. Let snapshotChangeCount be the current clipboard change count.
        // FIXME: We are meant to independently track any changes to the system clipboard in the UI. We are also
        //        meant to only fetch metadata about the system clipboard at this point, and defer reading the
        //        actual data until ClipboardItem.getType() is invoked.
        let snapshot_change_count = 0;

        // 5. Let items be a sequence of ordered maps, each entry having keys "item" (a clipboard item) and
        //    "originating" (a system clipboard item or null).
        let items = READ_ITEMS_CLASS.allocate(vm, |_| ReadItems {
            request,
            snapshot_change_count,
            items: GcRefCell::new(Vec::new()),
        });

        // 6. For each systemClipboardItem in data:
        for system_clipboard_item in data {
            // 1. Let item be a new clipboard item.
            let item = ClipboardItem::create(vm);

            // 2. For each systemClipboardRepresentation in systemClipboardItem:
            for system_clipboard_representation in &system_clipboard_item.system_clipboard_representations {
                // 1. Let mimeType be the result of running the well-known mime type from os specific format
                //    algorithm given systemClipboardRepresentation’s name.
                let mime_type = os_specific_well_known_format_of_string(&system_clipboard_representation.name);

                // 2. If mimeType is null, continue this loop.
                if mime_type.is_empty() {
                    continue;
                }

                // 3. Let representation be a new representation.
                let representation = Representation {
                    // 4. Set representation’s MIME type to mimeType.
                    mime_type: Utf16String::from_utf8(&mime_type),
                    is_custom: false,

                    // 5. Set representation’s data to a new promise in realm.
                    data: create_promise(vm, realm),
                };

                // 6. Append representation to item’s list of representations.
                item.append_representation(vm, representation);
            }

            // 3. If item’s list of representations size is greater than 0, append the ordered map
            //    «[ "item" → item, "originating" → systemClipboardItem ]» to items.
            if item.has_representations() {
                items.items.borrow_mut(vm).push(ReadItem {
                    item,
                    originating: Some(system_clipboard_item),
                });
            }
        }

        // 7. If items has a size > 0, then:
        if !items.items.borrow().is_empty() {
            // FIXME: 1. Let firstItem be items[0]["item"]
            // FIXME: 2. Run the read web custom format algorithm given firstItem.
        }
        // 8. Else:
        else {
            // FIXME: 1. Let customItem be a new clipboard item.
            // FIXME: 2. Run the read web custom format algorithm given customItem.
            // FIXME: 3. If customItem’s list of representations size is greater than 0, append the ordered map
            //           «[ "item" → customItem, "originating" → null ]» to items.
        }

        // 9. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
        queue_a_global_task(vm, realm.global_object(), TaskSource::Clipboard, items, |vm, items| {
            let realm = items.request.realm;
            with_temporary_execution_context(realm, || {
                // 1. Let clipboardItems be a sequence<ClipboardItem>.
                let clipboard_items = MarkedVec::new(vm);

                // 2. For each ordered map entry of items:
                for entry in items.items.borrow().iter() {
                    // 1. Let clipboardItem be the result of running the steps of create a ClipboardItem object given
                    //    entry["item"] and realm.
                    let clipboard_item = entry.item;

                    // 2. Set clipboardItem’s clipboard change count at read to snapshotChangeCount.
                    clipboard_item.set_clipboard_change_count_at_read(items.snapshot_change_count);

                    // 3. Set clipboardItem’s originating system clipboard item to entry["originating"].
                    clipboard_item.set_originating_system_clipboard_item(entry.originating.clone());

                    // 4. If formats is not empty, set clipboardItem’s unsanitized MIME types to formats["unsanitized"].
                    if let Some(unsanitized) = &items.request.formats.unsanitized {
                        clipboard_item.set_unsanitized_mime_types(unsanitized.clone());
                    }

                    // 5. Append clipboardItem to clipboardItems.
                    clipboard_items.push(clipboard_item);
                }

                // 3. Resolve p with clipboardItems.
                let cx = CallContext::new(vm, realm);
                let clipboard_items =
                    rooted_sequence_to_js(&cx, clipboard_items, |item| Ok(Value::from_object(wrap(&cx, *item))))
                        .expect("converting clipboard items does not throw");
                resolve_promise(vm, items.request.promise, clipboard_items);
            });
        });
    });
}

// NB: The steps of readText() after step 3 run once the system clipboard data is in hand.
fn read_text_steps_with_data(vm: &Vm, request: Gc<EmbedderCell<ReadTextRequest>>, data: Vec<SystemClipboardItem>) {
    let (realm, promise) = (request.realm, request.promise);

    // 4. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
    queue_a_global_task(
        vm,
        realm.global_object(),
        TaskSource::Clipboard,
        (realm, promise, data),
        |vm, (realm, promise, data)| {
            with_temporary_execution_context(realm, || {
                // 1. For each systemClipboardItem in data:
                for system_clipboard_item in &data {
                    // 1. For each systemClipboardRepresentation in systemClipboardItem:
                    for system_clipboard_representation in &system_clipboard_item.system_clipboard_representations {
                        // 1. Let mimeType be the result of running the well-known mime type from os specific format
                        //    algorithm given systemClipboardRepresentation’s name.
                        let mime_type = os_specific_well_known_format_of_string(&system_clipboard_representation.name);

                        // 2. If mimeType is null, continue this loop.
                        if mime_type.is_empty() {
                            continue;
                        }

                        // 3. Let representation be a new representation.
                        // FIXME: Spec issue: Creating a new representation here and reacting to its promise does not
                        //        make sense. Nothing will ever fulfill or reject its promise. So we resolve the outer
                        //        promise with the system clipboard data converted to UTF-8 instead. See:
                        //        https://github.com/w3c/clipboard-apis/issues/236

                        // 4. If representation’s MIME type essence is "text/plain", then:
                        if mime_type == "text/plain" {
                            // 1. Set representation’s MIME type to mimeType.
                            // 2. Let representationDataPromise be the representation’s data.

                            // 3. React to representationDataPromise:
                            //     1. If representationDataPromise was fulfilled with value v, then:
                            //         1. If v is a DOMString, then follow the below steps:
                            //             1. Resolve p with v.
                            //             2. Return p.
                            //         2. If v is a Blob, then follow the below steps:
                            //             1. Let string be the result of UTF-8 decoding v’s underlying byte sequence.
                            //             2. Resolve p with string.
                            //             3. Return p.
                            //     2. If representationDataPromise was rejected, then:
                            //         1. Reject p with "NotFoundError" DOMException in realm.
                            //         2. Return p.

                            let (string, _, _) = encoding_rs::UTF_8.decode(&system_clipboard_representation.data);
                            let string = PrimitiveString::create(vm, Utf16String::from_utf8(&string));
                            resolve_promise(vm, promise, Value::from_string(string));
                            return;
                        }
                    }
                }

                // 2. Reject p with "NotFoundError" DOMException in realm.
                reject_promise_with_exception(
                    vm,
                    promise,
                    Exception::Dom(
                        "NotFoundError",
                        Utf16String::from_utf8("Did not find a text item in the system clipboard"),
                    ),
                );
            });
        },
    );
}

// https://w3c.github.io/clipboard-apis/#dom-clipboard-write
fn write_task(vm: &Vm, data: Gc<EmbedderCell<WriteData>>) {
    let realm = data.realm;
    let promise = data.promise;

    // 1. Let itemList and cleanItemList be an empty sequence<Blob>.
    // FIXME: Spec issue: The spec does not clear itemList and cleanItemList in the outer `for` loop below. This
    //        will cause us to re-write the same items after the first iteration. So we defer creating these
    //        lists to prevent this. See:
    //        https://github.com/w3c/clipboard-apis/issues/237

    // 2. Let dataList be a sequence<ClipboardItem>.
    // 3. If data’s size is greater than 1, and the current operating system does not support multiple native
    //    clipboard items on the system clipboard, then add data[0] to dataList, else, set dataList to data.
    let data_list = data;

    // 4. For each clipboardItem in dataList:
    for &clipboard_item in &data_list.data {
        // NB: The reaction steps append to itemList, which these steps read after them, so it is a cell they share.
        let item_list = ITEM_LIST_CLASS.allocate(vm, |_| ItemList {
            realm,
            promise,
            blobs: GcRefCell::new(Vec::new()),
        });
        let clean_item_list = MarkedVec::new(vm);

        // 1. For each representation in clipboardItem’s clipboard item's list of representations:
        let representations = clipboard_item.representations(vm);
        for (index, representation) in representations.values().iter().enumerate() {
            // 1. Let representationDataPromise be the representation’s data.
            let representation_data_promise = representation.data;

            // 2. React to representationDataPromise:
            let reaction = react_to_promise(
                vm,
                representation_data_promise,
                (item_list, clipboard_item, index),
                // 1. If representationDataPromise was fulfilled with value v, then:
                Some(|cx, (item_list, clipboard_item, index), value| {
                    let vm = cx.vm();

                    // 1. If v is a DOMString, then follow the below steps:
                    if value.is_string() {
                        // 1. Let dataAsBytes be the result of UTF-8 encoding v.
                        let data_as_bytes =
                            String::from_utf16_lossy(&value.as_string().utf16_string().to_utf16()).into_bytes();

                        // 2. Let blobData be a Blob created using dataAsBytes with its type set to representation’s MIME type.
                        let mime_type = clipboard_item
                            .representations(vm)
                            .get(index)
                            .expect("the representation exists")
                            .mime_type;
                        let blob_data = allocate(vm, |_| Blob::new(data_as_bytes, mime_type));

                        // 3. Add blobData to itemList.
                        item_list.blobs.borrow_mut(vm).push(AnyBlob::Blob(blob_data));
                    }
                    // 2. If v is a Blob, then add v to itemList.
                    else if value.is_object()
                        && let Some(blob) = clipboard_item_data_blob(value)
                    {
                        item_list.blobs.borrow_mut(vm).push(blob);
                    }

                    Ok(Value::UNDEFINED)
                }),
                // 2. If representationDataPromise was rejected, then:
                Some(|cx, (item_list, _, _), reason| {
                    let vm = cx.vm();
                    with_temporary_execution_context(item_list.realm, || {
                        // 1. Reject p with "NotAllowedError" DOMException in realm.
                        let reason = reason.to_utf16_string_without_side_effects();
                        reject_promise_with_exception(
                            vm,
                            item_list.promise,
                            Exception::Dom(
                                "NotAllowedError",
                                Utf16String::from_utf8(&format!(
                                    "Writing to the clipboard failed: {}",
                                    String::from_utf16_lossy(&reason.to_utf16())
                                )),
                            ),
                        );
                    });

                    // 2. Abort these steps.
                    // NOTE: This is handled below.

                    Ok(Value::UNDEFINED)
                }),
            );

            // FIXME: Spec issue: The spec assumes the reaction steps above occur synchronously. This is never
            //        the case; even if the promise is already settled, the reaction jobs are queued as microtasks.
            //        https://github.com/w3c/clipboard-apis/issues/237
            spin_until_promise_is_settled(reaction);

            // NB: The reaction promise is fulfilled even when the steps above abort, so check representationDataPromise.
            if representation_data_promise
                .promise()
                .downcast::<Promise>()
                .expect("a representation's data is a Promise")
                .state()
                == PromiseState::Rejected
            {
                return;
            }
        }

        // 2. For each blob in itemList:
        for blob in item_list.blobs.borrow().iter() {
            // 1. Let type be the blob’s type.
            let r#type = blob.type_string();

            // 2. If type is not in the mandatory data types or optional data types list, then reject p with
            //    "NotAllowedError" DOMException in realm and abort these steps.
            if !ClipboardItem::supports_type(r#type) {
                reject_promise_with_exception(
                    vm,
                    promise,
                    Exception::Dom(
                        "NotAllowedError",
                        Utf16String::from_utf8(&format!(
                            "Clipboard item type {} is not allowed",
                            String::from_utf16_lossy(&r#type.to_utf16())
                        )),
                    ),
                );
                return;
            }

            // 3. Let cleanItem be an optionally sanitized copy of blob.
            let clean_item = blob.clone();

            // FIXME: 4. If sanitization was attempted and was not successfully completed, then follow the below steps:
            //     1. Reject p with "NotAllowedError" DOMException in realm.
            //     2. Abort these steps.

            // 5. Append cleanItem to cleanItemList.
            clean_item_list.push(clean_item);
        }

        // 3. Let option be clipboardItem’s clipboard item's presentation style.
        let option = clipboard_item.presentation_style.get().as_str();

        // 4. Write blobs and option to the clipboard with cleanItemList and option.
        write_blobs_and_option_to_clipboard(&relevant_window(realm), &clean_item_list.values(), option);
    }

    // 5. Resolve p.
    resolve_promise(vm, promise, Value::UNDEFINED);
}
