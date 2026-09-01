/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/Clipboard/ClipboardItem.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Clipboard {

GC_DEFINE_ALLOCATOR(ClipboardItem);

static void resolve_clipboard_item_blob_promise(JS::Realm& realm, WebIDL::Promise const& promise, GC::Ref<FileAPI::Blob> blob)
{
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, blob));
}

StringView presentation_style_to_string(PresentationStyle presentation_style)
{
    switch (presentation_style) {
    case PresentationStyle::Unspecified:
        return "unspecified"sv;
    case PresentationStyle::Inline:
        return "inline"sv;
    case PresentationStyle::Attachment:
        return "attachment"sv;
    }
    VERIFY_NOT_REACHED();
}

GC::Ref<ClipboardItem> ClipboardItem::create()
{
    return GC::Heap::the().allocate<ClipboardItem>();
}

// https://w3c.github.io/clipboard-apis/#dom-clipboarditem-clipboarditem
WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> ClipboardItem::create(GC::OrderedRootHashMap<Utf16String, GC::Ref<WebIDL::Promise>> const& items, PresentationStyle presentation_style)
{
    // 1. If items is empty, then throw a TypeError.
    if (items.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Items cannot be empty"_utf16 };

    // 2. If options is empty, then set options["presentationStyle"] = "unspecified".
    // NOTE: This step is handled by presentationStyle's default value in ClipboardItemOptions.

    // 3. Set this's clipboard item to a new clipboard item.
    auto clipboard_item = create();

    // 4. Set this's clipboard item's presentation style to options["presentationStyle"].
    clipboard_item->m_presentation_style = presentation_style;

    // 5. Let types be a list of DOMString.
    Vector<Utf16String> types;

    // 6. For each (key, value) in items:
    for (auto const& [key, value] : items) {
        // 2. Let isCustom be false.
        bool is_custom = false;

        // 3. If key starts with `"web "` prefix, then:
        auto key_without_prefix = key.utf16_view();
        if (key_without_prefix.starts_with(WEB_CUSTOM_FORMAT_PREFIX)) {
            // 1. Remove `"web "` prefix and assign the remaining string to key.
            key_without_prefix = key_without_prefix.substring_view(WEB_CUSTOM_FORMAT_PREFIX.length_in_code_units());

            // 2. Set isCustom to true.
            is_custom = true;
        }

        // 5. Let mimeType be the result of parsing a MIME type given key.
        auto mime_type = MimeSniff::MimeType::parse(key_without_prefix);

        // 6. If mimeType is failure, then throw a TypeError.
        if (!mime_type.has_value()) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Invalid MIME type: {}", key) };
        }

        auto mime_type_serialized = mime_type->serialized();
        auto mime_type_serialized_utf16 = Utf16String::from_utf8(mime_type_serialized);

        // 7. If this's clipboard item's list of representations contains a representation whose MIME type
        //    is mimeType and whose [representation/isCustom] is isCustom, then throw a TypeError.
        auto existing = clipboard_item->m_representations.find_if([&](auto const& item) {
            return item.mime_type == mime_type_serialized_utf16 && item.is_custom == is_custom;
        });
        if (!existing.is_end()) {
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Duplicate MIME type: {}", key) };
        }

        // 11. Let mimeTypeString be the result of serializing a MIME type with mimeType.
        // 12. If isCustom is true, prefix mimeTypeString with `"web "`.
        auto mime_type_string = is_custom ? Utf16String::formatted("web {}", mime_type_serialized_utf16) : mime_type_serialized_utf16;

        // 13. Add mimeTypeString to types.
        types.append(move(mime_type_string));

        // 1. Let representation be a new representation.
        // 4. Set representation’s isCustom flag to isCustom.
        // 8. Set representation’s MIME type to mimeType.
        // 9. Set representation’s data to value.
        // 10. Append representation to this's clipboard item's list of representations.
        clipboard_item->m_representations.empend(move(mime_type_serialized_utf16), is_custom, *value);
    }

    // 7. Set this's types array to the result of running create a frozen array from types.
    clipboard_item->m_types = types;

    return clipboard_item;
}

WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> ClipboardItem::create(GC::OrderedRootHashMap<Utf16String, GC::Ref<WebIDL::Promise>> const& items, Bindings::ClipboardItemOptions const& options)
{
    return create(items, options.presentation_style);
}

ClipboardItem::ClipboardItem()
    : m_presentation_style(PresentationStyle::Unspecified)
{
}

ClipboardItem::~ClipboardItem() = default;

void ClipboardItem::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& representation : m_representations)
        visitor.visit(representation.data);
    visitor.visit(m_representations_with_resolvers);
}

void ClipboardItem::append_representation(Representation representation)
{
    m_types.append(representation.mime_type);
    m_representations.append(move(representation));
}

// https://w3c.github.io/clipboard-apis/#dom-clipboarditem-gettype
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> ClipboardItem::get_type(JS::Realm& realm, Utf16String const& type)
{
    // 2. Let isCustom be false.
    bool is_custom = false;

    // 3. If type starts with `"web "` prefix, then:
    auto type_without_prefix = type.utf16_view();
    if (type_without_prefix.starts_with(WEB_CUSTOM_FORMAT_PREFIX)) {
        // 1. Remove `"web "` prefix and assign the remaining string to type.
        type_without_prefix = type_without_prefix.substring_view(WEB_CUSTOM_FORMAT_PREFIX.length_in_code_units());

        // 2. Set isCustom to true.
        is_custom = true;
    }

    // 4. Let mimeType be the result of parsing a MIME type given type.
    auto mime_type = MimeSniff::MimeType::parse(type_without_prefix);

    // 5. If mimeType is failure, then throw a TypeError.
    if (!mime_type.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::formatted("Invalid MIME type: {}", type) };

    auto mime_type_serialized = mime_type->serialized();

    // 6. Let itemTypeList be this’s clipboard item’s list of representations.
    auto const& item_type_list = representations();

    // 7.  Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 8. For each representation in itemTypeList:
    for (auto const& representation : item_type_list) {
        // 1. If representation’s MIME type is mimeType and representation’s isCustom is isCustom, then:
        if (representation.mime_type == mime_type_serialized && representation.is_custom == is_custom) {
            // FIXME: 1. If this’s clipboard change count at read is not null, and the current clipboard change count is not
            //           equal to this’s clipboard change count at read, then reject p with an "InvalidStateError" DOMException
            //           in realm, and return p.

            // 2. If this’s clipboard change count at read is not null, then:
            if (m_clipboard_change_count_at_read.has_value()) {
                // 1. Let key be mimeType’s essence. If isCustom is true, prefix key with `"web "`.
                auto key = mime_type->essence();
                if (is_custom)
                    key = MUST(String::formatted("web {}", mime_type->essence()));

                // 2. If this’s representations with resolvers[key] exists, then return this’s representations with resolvers[key].
                if (auto resolver = m_representations_with_resolvers.get(key); resolver.has_value())
                    return resolver.release_value();

                // 3. Set this’s representations with resolvers[key] to p.
                m_representations_with_resolvers.set(key, promise);

                // 4. Run the following steps in parallel:
                Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [this, &realm, promise, mime_type = mime_type.release_value(), is_custom]() {
                    // 1. Let clipboardItem be this’s originating system clipboard item.
                    auto const& clipboard_item = m_originating_system_clipboard_item;

                    // 2. If clipboardItem is null, then queue a global task on the clipboard task source, given realm’s
                    //    global object, to reject p with an "InvalidStateError" DOMException in realm, then abort these
                    //    steps.
                    if (!clipboard_item.has_value()) {
                        queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(GC::Heap::the(), [&realm, promise]() {
                            HTML::TemporaryExecutionContext execution_context { realm };
                            WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("This clipboard item is missing an originating system clipboard entry"_utf16));
                        }));
                        return;
                    }

                    String os_format_name;

                    // 3. If isCustom is true, then:
                    if (is_custom) {
                        // FIXME: 1. Let mapName be the os specific custom map name.
                        // FIXME: 2. Let mapRepresentation be the system clipboard representation in clipboardItem’s list of system
                        //           clipboard representations whose name is mapName. If there is no such system clipboard
                        //           representation, then queue a global task on the clipboard task source, given realm’s global
                        //           object, to reject p with a "NotFoundError" DOMException in realm, then abort these steps.
                        // FIXME: 3. Let webCustomFormatMapString be the JSON string deserialized from mapRepresentation’s data.
                        // FIXME: 4. Let osFormatName be the value in webCustomFormatMapString whose key matches mimeType
                        //           serialized.
                        // FIXME: 5. If osFormatName is not found, then queue a global task on the clipboard task source, given
                        //           realm’s global object, to reject p with a "NotFoundError" DOMException in realm, then abort
                        //           these steps.
                    }
                    // 4. Else, let osFormatName be the result of running os specific well-known format given mimeType.
                    else {
                        os_format_name = os_specific_well_known_format(mime_type);
                    }

                    // 5. Let clipboardRepresentation be the system clipboard representation in clipboardItem’s list of
                    //    system clipboard representations whose name is osFormatName. If there is no such system clipboard
                    //    representation, then queue a global task on the clipboard task source, given realm’s global object,
                    //    to reject p with a "NotFoundError" DOMException in realm, then abort these steps.
                    auto clipboard_representation = clipboard_item->system_clipboard_representations.first_matching([&](auto const& item) {
                        return item.name == os_format_name;
                    });

                    if (!clipboard_representation.has_value()) {
                        queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(GC::Heap::the(), [&realm, promise, os_format_name]() {
                            HTML::TemporaryExecutionContext execution_context { realm };
                            WebIDL::reject_promise(promise, WebIDL::NotFoundError::create(realm, Utf16String::formatted("No data found for MIME type: {}", os_format_name)));
                        }));
                        return;
                    }

                    // 6. Let rawData be clipboardRepresentation’s data.
                    auto const& raw_data = clipboard_representation->data;

                    // 7. Let cleanData be a copy of rawData.
                    auto clean_data = MUST(ByteBuffer::copy(raw_data.bytes()));

                    // FIXME: 8. If mimeType’s essence is in this’s unsanitized MIME types and mimeType’s essence is in the
                    //           optional unsanitized data types list, then do nothing.
                    // FIXME: 9. Else, if mimeType’s essence is not "image/png", the user agent MAY sanitize cleanData.

                    // 10. Let blob be a Blob whose type is mimeType serialized and whose underlying byte sequence is cleanData.
                    auto blob = FileAPI::Blob::create(move(clean_data), mime_type.serialized_as_utf16());

                    // 11. Queue a global task on the clipboard task source, given realm’s global object, to resolve p with blob.
                    queue_global_task(HTML::Task::Source::Clipboard, realm.global_object(), GC::create_function(GC::Heap::the(), [&realm, promise, blob]() {
                        HTML::TemporaryExecutionContext execution_context { realm };
                        resolve_clipboard_item_blob_promise(realm, promise, blob);
                    }));
                }));
            }

            // 3. Let representationDataPromise be the representation’s data.
            auto representation_data_promise = representation.data;

            // 4. React to representationDataPromise:
            WebIDL::react_to_promise(
                *representation_data_promise,
                // 1. If representationDataPromise was fulfilled with value v, then:
                GC::create_function(GC::Heap::the(), [&realm, promise, mime_type_serialized](JS::Value value) -> WebIDL::ExceptionOr<JS::Value> {
                    // 1. If v is a DOMString, then follow the below steps:
                    if (value.is_string()) {
                        // 1. Let dataAsBytes be the result of UTF-8 encoding v.
                        auto utf8_string = value.as_string().utf16_string().to_utf8_but_should_be_ported_to_utf16();
                        auto data_as_bytes = MUST(ByteBuffer::copy(utf8_string.bytes()));

                        // 2. Let blobData be a Blob created using dataAsBytes with its type set to mimeType, serialized.
                        auto blob_data = FileAPI::Blob::create(data_as_bytes, mime_type_serialized);

                        // 3. Resolve p with blobData.
                        resolve_clipboard_item_blob_promise(realm, promise, blob_data);
                    }
                    // 2. If v is a Blob, then follow the below steps:
                    if (value.is_object() && Bindings::impl_from<FileAPI::Blob>(&value.as_object())) {
                        // 1. Resolve p with v.
                        WebIDL::resolve_promise(promise, value);
                    }

                    return JS::js_undefined();
                }),
                // 2. If representationDataPromise was rejected, then:
                GC::create_function(GC::Heap::the(), [&realm, type, promise](JS::Value) -> WebIDL::ExceptionOr<JS::Value> {
                    // 1. Reject p with "NotFoundError" DOMException in realm.
                    WebIDL::reject_promise(promise, WebIDL::NotFoundError::create(realm, Utf16String::formatted("No data found for MIME type: {}", type)));

                    return JS::js_undefined();
                }));

            // 5. Return p.
            return promise;
        }
    }

    // 9. Reject p with "NotFoundError" DOMException in realm.
    WebIDL::reject_promise(promise, WebIDL::NotFoundError::create(realm, Utf16String::formatted("No data found for MIME type: {}", type)));

    // 10. Return p.
    return promise;
}

// https://w3c.github.io/clipboard-apis/#dom-clipboarditem-supports
bool ClipboardItem::supports(Utf16String const& type)
{
    // 1. If type is in mandatory data types or optional data types, then return true.
    // 2. If not, then return false.
    // TODO: Implement optional data types, like web custom formats and image/svg+xml.
    return type == "text/plain"_utf16 || type == "text/html"_utf16 || type == "image/png"_utf16;
}

}
