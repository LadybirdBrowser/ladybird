/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/MimeTypeArray.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(MimeTypeArray);

GC::Ref<MimeTypeArray> MimeTypeArray::create(Window& window)
{
    return GC::Heap::the().allocate<MimeTypeArray>(window);
}

MimeTypeArray::MimeTypeArray(Window& window)
    : m_window(window)
{
}

MimeTypeArray::~MimeTypeArray() = default;

void MimeTypeArray::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties-2
Vector<FlyString> MimeTypeArray::supported_property_names() const
{
    // The MimeTypeArray interface supports named properties. If the user agent's PDF viewer supported is true, then they are the PDF viewer mime types. Otherwise, they are the empty list.
    if (!m_window->page().pdf_viewer_supported())
        return {};

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-mime-types
    static NeverDestroyed<Vector<FlyString>> mime_types { Vector<FlyString> {
        "application/pdf"_fly_string,
        "text/pdf"_fly_string,
    } };

    return *mime_types;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetypearray-length
size_t MimeTypeArray::length() const
{
    // The MimeTypeArray interface's length getter steps are to return this's relevant global object's PDF viewer mime type objects's size.
    return m_window->pdf_viewer_mime_type_objects().size();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetypearray-item
GC::Ptr<MimeType> MimeTypeArray::item(u32 index) const
{
    // 1. Let mimeTypes be this's relevant global object's PDF viewer mime type objects.
    auto mime_types = m_window->pdf_viewer_mime_type_objects();

    // 2. If index < mimeTypes's size, then return mimeTypes[index].
    if (index < mime_types.size())
        return mime_types[index];

    // 3. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-mimetypearray-nameditem
GC::Ptr<MimeType> MimeTypeArray::named_item(FlyString const& name) const
{
    // 1. For each MimeType mimeType of this's relevant global object's PDF viewer mime type objects: if mimeType's type is name, then return mimeType.
    auto mime_types = m_window->pdf_viewer_mime_type_objects();

    for (auto& mime_type : mime_types) {
        if (mime_type->type() == name)
            return mime_type;
    }

    // 2. Return null.
    return nullptr;
}

}
