/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/Bindings/ClipboardPrototype.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Clipboard {

JS_DEFINE_ALLOCATOR(Clipboard);

WebIDL::ExceptionOr<JS::NonnullGCPtr<Clipboard>> Clipboard::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<Clipboard>(realm, realm);
}

Clipboard::Clipboard(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

Clipboard::~Clipboard() = default;

void Clipboard::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Clipboard);
}

// https://w3c.github.io/clipboard-apis/#os-specific-well-known-format
static String os_specific_well_known_format(StringView mime_type_string)
{
    // NOTE: Here we always takes the Linux case, and defer to the chrome layer to handle OS specific implementations.
    auto mime_type = MimeSniff::MimeType::parse(mime_type_string);

    // 1. Let wellKnownFormat be an empty string.
    String well_known_format {};

    // 2. If mimeType’s essence is "text/plain", then
    if (mime_type->essence() == "text/plain"sv) {
        // On Windows, follow the convention described below:
        //     Assign CF_UNICODETEXT to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypeString to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/plain" to wellKnownFormat.
        well_known_format = "text/plain"_string;
    }
    // 3. Else, if mimeType’s essence is "text/html", then
    if (mime_type->essence() == "text/html"sv) {
        // On Windows, follow the convention described below:
        //     Assign CF_HTML to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSHTMLPboardType to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "text/html" to wellKnownFormat.
        well_known_format = "text/html"_string;
    }
    // 4. Else, if mimeType’s essence is "image/png", then
    if (mime_type->essence() == "image/png"sv) {
        // On Windows, follow the convention described below:
        //     Assign "PNG" to wellKnownFormat.
        // On MacOS, follow the convention described below:
        //     Assign NSPasteboardTypePNG to wellKnownFormat.
        // On Linux, ChromeOS, and Android, follow the convention described below:
        //     Assign "image/png" to wellKnownFormat.
        well_known_format = "image/png"_string;
    }

    // 5. Return wellKnownFormat.
    return well_known_format;
}

// https://w3c.github.io/clipboard-apis/#write-blobs-and-option-to-the-clipboard
static void write_blobs_and_option_to_clipboard(JS::Realm& realm, ReadonlySpan<JS::NonnullGCPtr<FileAPI::Blob>> items, String presentation_style)
{
    auto& window = verify_cast<HTML::Window>(realm.global_object());

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
        window.page().client().page_did_insert_clipboard_entry(move(payload), move(presentation_style), move(format_string));
    }

    // FIXME: 3. Write web custom formats given webCustomFormats.
}

// https://w3c.github.io/clipboard-apis/#check-clipboard-write-permission
static bool check_clipboard_write_permission(JS::Realm& realm)
{
    // NOTE: The clipboard permission is undergoing a refactor because the clipboard-write permission was removed from
    //       the Permissions spec. So this partially implements the proposed update:
    //       https://pr-preview.s3.amazonaws.com/w3c/clipboard-apis/pull/164.html#write-permission

    // 1. Let hasGesture be true if the relevant global object of this has transient activation, false otherwise.
    auto has_gesture = verify_cast<HTML::Window>(realm.global_object()).has_transient_activation();

    // 2. If hasGesture then,
    if (has_gesture) {
        // FIXME: 1. Return true if the current script is running as a result of user interaction with a "cut" or "copy"
        //           element created by the user agent or operating system.
        return true;
    }

    // 3. Otherwise, return false.
    return false;
}

// https://w3c.github.io/clipboard-apis/#dom-clipboard-writetext
JS::NonnullGCPtr<JS::Promise> Clipboard::write_text(String data)
{
    // 1. Let realm be this's relevant realm.
    auto& realm = HTML::relevant_realm(*this);

    // 2. Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 3. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke([&realm, promise, data = move(data)]() mutable {
        // 1. Let r be the result of running check clipboard write permission.
        auto result = check_clipboard_write_permission(realm);

        // 2. If r is false, then:
        if (!result) {
            // 1. Queue a global task on the permission task source, given realm’s global object, to reject p with
            //    "NotAllowedError" DOMException in realm.
            queue_global_task(HTML::Task::Source::Permissions, realm.global_object(), JS::create_heap_function(realm.heap(), [&realm, promise]() mutable {
                HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(realm) };
                WebIDL::reject_promise(realm, promise, WebIDL::NotAllowedError::create(realm, "Clipboard writing is only allowed through user activation"_string));
            }));

            // 2. Abort these steps.
            return;
        }

        // 1. Queue a global task on the clipboard task source, given realm’s global object, to perform the below steps:
        queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), JS::create_heap_function(realm.heap(), [&realm, promise, data = move(data)]() mutable {
            // 1. Let itemList be an empty sequence<Blob>.
            Vector<JS::NonnullGCPtr<FileAPI::Blob>> item_list;

            // 2. Let textBlob be a new Blob created with: type attribute set to "text/plain;charset=utf-8", and its
            //    underlying byte sequence set to the UTF-8 encoding of data.
            //    Note: On Windows replace `\n` characters with `\r\n` in data before creating textBlob.
            auto text_blob = FileAPI::Blob::create(realm, MUST(ByteBuffer::copy(data.bytes())), "text/plain;charset=utf-8"_string);

            // 3. Add textBlob to itemList.
            item_list.append(text_blob);

            // 4. Let option be set to "unspecified".
            auto option = "unspecified"_string;

            // 5. Write blobs and option to the clipboard with itemList and option.
            write_blobs_and_option_to_clipboard(realm, item_list, move(option));

            // 6. Resolve p.
            HTML::TemporaryExecutionContext execution_context { Bindings::host_defined_environment_settings_object(realm) };
            WebIDL::resolve_promise(realm, promise, JS::js_undefined());
        }));
    });

    // 4. Return p.
    return JS::NonnullGCPtr { verify_cast<JS::Promise>(*promise->promise()) };
}

}
