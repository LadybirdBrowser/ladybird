/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/ClipboardItem.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Clipboard {

constexpr auto WEB_CUSTOM_FORMAT_PREFIX = "web "_utf16;

using PresentationStyle = Bindings::PresentationStyle;

StringView presentation_style_to_string(PresentationStyle);

// https://w3c.github.io/clipboard-apis/#clipboard-item-interface
class ClipboardItem : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(ClipboardItem, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(ClipboardItem);

public:
    struct Representation {
        Utf16String mime_type;         // The MIME type (e.g., "text/plain").
        bool is_custom { false };      // Whether this is a web custom format.
        GC::Ref<WebIDL::Promise> data; // The actual data for this representation.
    };

    static GC::Ref<ClipboardItem> create();
    static WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> create(GC::OrderedRootHashMap<Utf16String, GC::Ref<WebIDL::Promise>> const& items, PresentationStyle);
    static WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> create(GC::OrderedRootHashMap<Utf16String, GC::Ref<WebIDL::Promise>> const& items, Bindings::ClipboardItemOptions const&);

    virtual ~ClipboardItem() override;

    PresentationStyle presentation_style() const;

    Vector<Utf16String> const& types() const { return m_types; }

    Vector<Representation> const& representations() const { return m_representations; }
    void append_representation(Representation);
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> get_type(JS::Realm&, Utf16String const& type) const;

    static bool supports(Utf16String const& type);

private:
    ClipboardItem();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    PresentationStyle m_presentation_style;
    Vector<Utf16String> m_types;
    Vector<Representation> m_representations;
};

}
