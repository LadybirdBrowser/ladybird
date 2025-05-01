/*
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ClipboardPrototype.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/Clipboard/ClipboardItem.h>
#include <LibWeb/Clipboard/SystemClipboard.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Clipboard {

GC_DEFINE_ALLOCATOR(Clipboard);

WebIDL::ExceptionOr<GC::Ref<Clipboard>> Clipboard::construct_impl(JS::Realm& realm)
{
    return realm.create<Clipboard>(realm);
}

Clipboard::Clipboard(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

Clipboard::~Clipboard() = default;

void Clipboard::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Clipboard);
    Base::initialize(realm);
}

// https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
static String os_specific_well_known_format(StringView mime_type_string)
{
    // NOTE: Here we always takes the Linux case, and defer to the browser process to handle OS specific implementations.
    auto mime_type = MimeSniff::MimeType::parse(mime_type_string);

    // 1. Let wellKnownFormat be an empty string.
    String well_known_format {};

    // 2. If mimeType’s essence is "text/plain", then
    if (auto const& essence = mime_type->essence(); essence == "text/plain"sv) {
        // On Windows, follow the convention described below:
        //     Assign CF_UNICODETEXT to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypeString to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/plain" to wellKnownFormat.
        well_known_format = essence;
    }
    // 3. Else, if mimeType’s essence is "text/html", then
    else if (essence == "text/html"sv) {
        // On Windows, follow the convention described below:
        //     Assign CF_HTML to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSHTMLPboardType to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/html" to wellKnownFormat.
        well_known_format = essence;
    }
    // 4. Else, if mimeType’s essence is "image/png", then
    else if (essence == "image/png"sv) {
        // On Windows, follow the convention described below:
        //     Assign "PNG" to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypePNG to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "image/png" to wellKnownFormat.
        well_known_format = essence;
    }

    // 5. Return wellKnownFormat.
    return well_known_format;
}

// https://w3c.github.io/clipboard-apis/#write-blobs-and-option-to-the-clipboard
static void write_blobs_and_option_to_clipboard(JS::Realm& realm, ReadonlySpan<GC::Ref<FileAPI::Blob>> items, StringView presentation_style)
{
    auto& window = as<HTML::Window>(realm.global_object());

    // FIXME: 1. Let webCustomFormats be a sequence<Blob>.

    // 2. For each item in items:
    for (auto const& item : items) {
        // 1. Let formatString be the result of running os specific well-known format given item’s type.
        auto format_string = os_specific_well_known_format(item->type());

        // 2. If formatString is empty then follow the below steps:
        if (format_string.is_empty()) {
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
        auto decoder = TextCodec::decoder_for("UTF-8"sv);
        auto payload = MUST(TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, item->raw_bytes()));

        // 4. Insert payload and presentationStyle into the system clipboard using formatString as the native clipboard format.
        window.page().client().page_did_insert_clipboard_entry({ payload.to_byte_string(), move(format_string) }, presentation_style);
    }

    // FIXME: 3. Write web custom formats given webCustomFormats.
}

// https://w3c.github.io/clipboard-apis/#h-clipboard-read-permission
static bool check_clipboard_read_permission(JS::Realm& realm)
{
    // NOTE: The clipboard permission is undergoing a refactor because the clipboard-read permission was removed from
    //       the Permissions spec. So this partially implements the proposed update:
    //       https://pr-preview.s3.amazonaws.com/w3c/clipboard-apis/pull/164.html#read-permission

    // 1. Let hasGesture be true if the relevant global object of this has transient activation, false otherwise.
    auto has_gesture = as<HTML::Window>(realm.global_object()).has_transient_activation();

    // 2. If hasGesture then,
    if (has_gesture) {
        // FIXME: 1. Return true if the current script is running as a result of user interaction with a "Paste" element
        //           created by the user agent or operating system.
        return true;
    }

    // 3. Otherwise, return false.
    return false;
}

// https://w3c.github.io/clipboard-apis/#check-clipboard-write-permission
static bool check_clipboard_write_permission(JS::Realm& realm)
{
    // NOTE: The clipboard permission is undergoing a refactor because the clipboard-write permission was removed from
    //       the Permissions spec. So this partially implements the proposed update:
    //       https://pr-preview.s3.amazonaws.com/w3c/clipboard-apis/pull/164.html#write-permission

    // 1. Let hasGesture be true if the relevant global object of this has transient activation, false otherwise.
    auto has_gesture = as<HTML::Window>(realm.global_object()).has_transient_activation();

    // 2. If hasGesture then,
    if (has_gesture) {
        // FIXME: 1. Return true if the current script is running as a result of user interaction with a "cut" or "copy"
        //           element created by the user agent or operating system.
        return true;
    }

    // 3. Otherwise, return false.
    return false;
}

// https://w3c.github.io/clipboard-apis/#dom-clipboard-readtext
GC::Ref<WebIDL::Promise> Clipboard::read_text()
{
    // 1. Let realm be this's relevant realm.
    auto& realm = HTML::relevant_realm(*this);

    // 2. Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 3. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, promise]() mutable {
        // 1. Let r be the result of running check clipboard read permission.
        auto result = check_clipboard_read_permission(realm);

        // 2. If r is false, then:
        if (!result) {
            // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
            //    "NotAllowedError" DOMException in realm.
            queue_global_task(HTML::Task::Source::Permissions, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() mutable {
                HTML::TemporaryExecutionContext execution_context { realm };
                WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Clipboard reading is only allowed through user activation"_string));
            }));

            // 2. Abort these steps.
            return;
        }

        // 3. Let data be a copy of the system clipboard data.
        as<HTML::Window>(realm.global_object()).page().request_clipboard_entries(GC::create_function(realm.heap(), [&realm, promise](Vector<SystemClipboardItem> data) mutable {
            // 4. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
            queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
                HTML::TemporaryExecutionContext execution_context { realm };

                // 1. For each systemClipboardItem in data:
                for (auto const& system_clipboard_item : data) {
                    // 1. For each systemClipboardRepresentation in systemClipboardItem:
                    for (auto const& system_clipboard_representation : system_clipboard_item.system_clipboard_representations) {
                        // 1. Let mimeType be the result of running the well-known mime type from os specific format
                        //    algorithm given systemClipboardRepresentation’s name.
                        auto mime_type = os_specific_well_known_format(system_clipboard_representation.mime_type);

                        // 2. If mimeType is null, continue this loop.
                        if (mime_type.is_empty())
                            continue;

                        // 3. Let representation be a new representation.
                        // FIXME: Spec issue: Creating a new representation here and reacting to its promise does not
                        //        make sense. Nothing will ever fulfill or reject its promise. So we resolve the outer
                        //        promise with the system clipboard data converted to UTF-8 instead. See:
                        //        https://github.com/w3c/clipboard-apis/issues/236

                        // 4. If representation’s MIME type essence is "text/plain", then:
                        if (mime_type == "text/plain"sv) {
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

                            auto decoder = TextCodec::decoder_for("UTF-8"sv);
                            auto string = MUST(TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, system_clipboard_representation.data));

                            WebIDL::resolve_promise(realm, promise, JS::PrimitiveString::create(realm.vm(), move(string)));
                            return;
                        }
                    }
                }

                // 2. Reject p with "NotFoundError" DOMException in realm.
                WebIDL::reject_promise(realm, promise, WebIDL::NotFoundError::create(realm, "Did not find a text item in the system clipboard"_string));
            }));
        }));
    }));

    // 5. Return p.
    return promise;
}

// https://w3c.github.io/clipboard-apis/#dom-clipboard-write
GC::Ref<WebIDL::Promise> Clipboard::write(GC::RootVector<GC::Root<ClipboardItem>>& data)
{
    // 1. Let realm be this's relevant realm.
    auto& realm = HTML::relevant_realm(*this);

    // 2. Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 3. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
        // 1. Let r be the result of running check clipboard write permission.
        auto result = check_clipboard_write_permission(realm);

        // 2. If r is false, then:
        if (!result) {
            // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
            //    "NotAllowedError" DOMException in realm.
            queue_global_task(HTML::Task::Source::Permissions, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() mutable {
                HTML::TemporaryExecutionContext execution_context { realm };
                WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Clipboard writing is only allowed through user activation"_string));
            }));

            // 2. Abort these steps.
            return;
        }

        // 3. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
        queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Let itemList and cleanItemList be an empty sequence<Blob>.
            // FIXME: Spec issue: The spec does not clear itemList and cleanItemList in the outer `for` loop below. This
            //        will cause us to re-write the same items after the first iteration. So we defer creating these
            //        lists to prevent this. See:
            //        https://github.com/w3c/clipboard-apis/issues/237

            // 2. Let dataList be a sequence<ClipboardItem>.
            // 3. If data’s size is greater than 1, and the current operating system does not support multiple native
            //    clipboard items on the system clipboard, then add data[0] to dataList, else, set dataList to data.
            auto data_list = move(data);

            // 4. For each clipboardItem in dataList:
            for (auto const& clipboard_item : data_list) {
                IGNORE_USE_IN_ESCAPING_LAMBDA GC::RootVector<GC::Ref<FileAPI::Blob>> item_list(realm.heap());
                GC::RootVector<GC::Ref<FileAPI::Blob>> clean_item_list(realm.heap());

                // 1. For each representation in clipboardItem’s clipboard item's list of representations:
                for (auto const& representation : clipboard_item->representations()) {
                    // 1. Let representationDataPromise be the representation’s data.
                    auto representation_data_promise = representation.data;

                    // 2. React to representationDataPromise:
                    auto reaction = WebIDL::react_to_promise(representation_data_promise,
                        // 1. If representationDataPromise was fulfilled with value v, then:
                        GC::create_function(realm.heap(), [&realm, &item_list, mime_type = representation.mime_type](JS::Value value) mutable -> WebIDL::ExceptionOr<JS::Value> {
                            // 1. If v is a DOMString, then follow the below steps:
                            if (value.is_string()) {
                                // 1. Let dataAsBytes be the result of UTF-8 encoding v.
                                auto const& data_as_bytes = value.as_string().utf8_string();

                                // 2. Let blobData be a Blob created using dataAsBytes with its type set to representation’s MIME type.
                                auto blob_data = FileAPI::Blob::create(realm, MUST(ByteBuffer::copy(data_as_bytes.bytes())), move(mime_type));

                                // 3. Add blobData to itemList.
                                item_list.append(blob_data);
                            }

                            // 2. If v is a Blob, then add v to itemList.
                            else if (value.is_object()) {
                                if (auto* blob = as_if<FileAPI::Blob>(value.as_object()))
                                    item_list.append(*blob);
                            }

                            return JS::js_undefined();
                        }),

                        // 2. If representationDataPromise was rejected, then:
                        GC::create_function(realm.heap(), [&realm, promise](JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
                            HTML::TemporaryExecutionContext execution_context { realm };

                            // 1. Reject p with "NotAllowedError" DOMException in realm.
                            WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, MUST(String::formatted("Writing to the clipboard failed: {}", reason))));

                            // 2. Abort these steps.
                            // NOTE: This is handled below.

                            return JS::js_undefined();
                        }));

                    // FIXME: Spec issue: The spec assumes the reaction steps above occur synchronously. This is never
                    //        the case; even if the promise is already settled, the reaction jobs are queued as microtasks.
                    //        https://github.com/w3c/clipboard-apis/issues/237
                    auto& reaction_promise = as<JS::Promise>(*reaction->promise());

                    HTML::main_thread_event_loop().spin_until(GC::create_function(realm.heap(), [&reaction_promise]() {
                        return reaction_promise.state() != JS::Promise::State::Pending;
                    }));

                    if (reaction_promise.state() == JS::Promise::State::Rejected)
                        return;
                }

                // 2. For each blob in itemList:
                for (auto blob : item_list) {
                    // 1. Let type be the blob’s type.
                    auto const& type = blob->type();

                    // 2. If type is not in the mandatory data types or optional data types list, then reject p with
                    //    "NotAllowedError" DOMException in realm and abort these steps.
                    if (!ClipboardItem::supports(realm.vm(), type)) {
                        WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, MUST(String::formatted("Clipboard item type {} is not allowed", type))));
                        return;
                    }

                    // 3. Let cleanItem be an optionally sanitized copy of blob.
                    auto clean_item = blob;

                    // FIXME: 4. If sanitization was attempted and was not successfully completed, then follow the below steps:
                    //     1. Reject p with "NotAllowedError" DOMException in realm.
                    //     2. Abort these steps.

                    // 5. Append cleanItem to cleanItemList.
                    clean_item_list.append(clean_item);
                }

                // 3. Let option be clipboardItem’s clipboard item's presentation style.
                auto option = Bindings::idl_enum_to_string(clipboard_item->presentation_style());

                // 4. Write blobs and option to the clipboard with cleanItemList and option.
                write_blobs_and_option_to_clipboard(realm, clean_item_list, option);
            }

            // 5. Resolve p.
            WebIDL::resolve_promise(realm, promise);
        }));
    }));

    // 4. Return p.
    return promise;
}

// https://w3c.github.io/clipboard-apis/#dom-clipboard-writetext
GC::Ref<WebIDL::Promise> Clipboard::write_text(String data)
{
    // 1. Let realm be this's relevant realm.
    auto& realm = HTML::relevant_realm(*this);

    // 2. Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 3. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
        // 1. Let r be the result of running check clipboard write permission.
        auto result = check_clipboard_write_permission(realm);

        // 2. If r is false, then:
        if (!result) {
            // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
            //    "NotAllowedError" DOMException in realm.
            queue_global_task(HTML::Task::Source::Permissions, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise]() mutable {
                HTML::TemporaryExecutionContext execution_context { realm };
                WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Clipboard writing is only allowed through user activation"_string));
            }));

            // 2. Abort these steps.
            return;
        }

        // 3. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
        queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
            // 1. Let itemList be an empty sequence<Blob>.
            GC::RootVector<GC::Ref<FileAPI::Blob>> item_list(realm.heap());

            // 2. Let textBlob be a new Blob created with: type attribute set to "text/plain;charset=utf-8", and its
            //    underlying byte sequence set to the UTF-8 encoding of data.
            //    Note: On Windows replace `\n` characters with `\r\n` in data before creating textBlob.
            auto text_blob = FileAPI::Blob::create(realm, MUST(ByteBuffer::copy(data.bytes())), "text/plain;charset=utf-8"_string);

            // 3. Add textBlob to itemList.
            item_list.append(text_blob);

            // 4. Let option be set to "unspecified".
            static constexpr auto option = "unspecified"sv;

            // 5. Write blobs and option to the clipboard with itemList and option.
            write_blobs_and_option_to_clipboard(realm, item_list, option);

            // 6. Resolve p.
            HTML::TemporaryExecutionContext execution_context { realm };
            WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        }));
    }));

    // 4. Return p.
    return promise;
}

}
