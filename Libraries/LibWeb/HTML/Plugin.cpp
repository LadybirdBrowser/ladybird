/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Plugin.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Plugin);

GC::Ref<Plugin> Plugin::create(Window& window, String name)
{
    return GC::Heap::the().allocate<Plugin>(window, move(name));
}

Plugin::Plugin(Window& window, String name)
    : m_name(Utf16FlyString::from_utf8(name.bytes()))
    , m_window(window)
{
}

Plugin::~Plugin() = default;

void Plugin::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-name
Utf16FlyString const& Plugin::name() const
{
    // The Plugin interface's name getter steps are to return this's name.
    return m_name;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-description
Utf16FlyString Plugin::description() const
{
    // The Plugin interface's description getter steps are to return "Portable Document Format".
    return "Portable Document Format"_utf16_fly_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-filename
Utf16FlyString Plugin::filename() const
{
    // The Plugin interface's filename getter steps are to return "internal-pdf-viewer".
    return "internal-pdf-viewer"_utf16_fly_string;
}

// https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewing-support:support-named-properties-3
Vector<Utf16FlyString> Plugin::supported_property_names() const
{
    // The Plugin interface supports named properties. If the user agent's PDF viewer supported is true, then they are the PDF viewer mime types. Otherwise, they are the empty list.
    if (!m_window->page().pdf_viewer_supported())
        return {};

    // https://html.spec.whatwg.org/multipage/system-state.html#pdf-viewer-mime-types
    static NeverDestroyed<Vector<Utf16FlyString>> mime_types { Vector<Utf16FlyString> {
        "application/pdf"_utf16_fly_string,
        "text/pdf"_utf16_fly_string,
    } };

    return *mime_types;
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-length
size_t Plugin::length() const
{
    // The Plugin interface's length getter steps are to return this's relevant global object's PDF viewer mime type objects's size.
    return m_window->pdf_viewer_mime_type_objects().size();
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-plugin-item
GC::Ptr<MimeType> Plugin::item(u32 index) const
{
    // 1. Let mimeTypes be this's relevant global object's PDF viewer mime type objects.
    auto mime_types = m_window->pdf_viewer_mime_type_objects();

    // 2. If index < mimeTypes's size, then return mimeTypes[index].
    if (index < mime_types.size())
        return mime_types[index];

    // 3. Return null.
    return nullptr;
}

GC::Ptr<MimeType> Plugin::named_item(Utf16FlyString const& name) const
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
