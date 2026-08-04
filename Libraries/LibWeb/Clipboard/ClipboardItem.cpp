/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Clipboard/ClipboardItem.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/MimeSniff/MimeType.h>
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

PresentationStyle ClipboardItem::presentation_style() const
{
    return m_presentation_style;
}

void ClipboardItem::append_representation(Representation representation)
{
    m_types.append(representation.mime_type);
    m_representations.append(move(representation));
}

// https://w3c.github.io/clipboard-apis/#dom-clipboarditem-gettype
WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> ClipboardItem::get_type(JS::Realm& realm, Utf16String const& type) const
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

    // 6. Let itemTypeList be this's clipboard item's list of representations.
    auto const& item_type_list = representations();

    // 7.  Let p be a new promise in realm.
    auto promise = WebIDL::create_promise(realm);

    // 8. For each representation in itemTypeList:
    for (auto const& representation : item_type_list) {
        // 1. If representation’s MIME type is mimeType and representation’s isCustom is isCustom, then:
        if (representation.mime_type == Utf16String::from_utf8(mime_type_serialized) && representation.is_custom == is_custom) {
            // 1. Let representationDataPromise be the representation’s data.
            auto representation_data_promise = representation.data;

            // 2. React to representationDataPromise:
            WebIDL::react_to_promise(
                *representation_data_promise,
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

            // 3. Return p.
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

ClipboardItem::ClipboardItem()
    : m_presentation_style(PresentationStyle::Unspecified)
{
}

ClipboardItem::~ClipboardItem() = default;

void ClipboardItem::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& representation : m_representations) {
        visitor.visit(representation.data);
    }
}

}
