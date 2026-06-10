/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/ClipboardItem.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/DataTransfer.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Clipboard {

constexpr auto WEB_CUSTOM_FORMAT_PREFIX = "web "sv;

// https://w3c.github.io/clipboard-apis/#mandatory-data-types
constexpr inline Array MANDATORY_DATA_TYPES = {
    "text/plain"sv, "text/html"sv, "image/png"sv
};

using PresentationStyle = Bindings::PresentationStyle;

StringView presentation_style_to_string(PresentationStyle);

// https://w3c.github.io/clipboard-apis/#clipboard-item-interface
class ClipboardItem : public Bindings::Wrappable {
    WEB_WRAPPABLE(ClipboardItem, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(ClipboardItem);

public:
    struct Representation {
        String mime_type;              // The MIME type (e.g., "text/plain").
        bool is_custom { false };      // Whether this is a web custom format.
        GC::Ref<WebIDL::Promise> data; // The actual data for this representation.
    };

    static GC::Ref<ClipboardItem> create();
    static WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> create(GC::OrderedRootHashMap<String, GC::Ref<WebIDL::Promise>> const& items, PresentationStyle);
    static WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> create(GC::OrderedRootHashMap<String, GC::Ref<WebIDL::Promise>> const& items, Bindings::ClipboardItemOptions const&);

    virtual ~ClipboardItem() override;

    PresentationStyle presentation_style() const;

    Vector<String> const& types() const { return m_types; }

    Vector<Representation> const& representations() const { return m_representations; }
    void append_representation(Representation);
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> get_type(JS::Realm&, String const& type) const;

    static bool supports(String const& type);

private:
    ClipboardItem();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    PresentationStyle m_presentation_style;
    Vector<String> m_types;
    Vector<Representation> m_representations;
};

}
