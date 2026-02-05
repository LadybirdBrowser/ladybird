/*
 * Copyright (c) 2024, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibWeb/Bindings/ClipboardItemPrototype.h>
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

struct ClipboardItemOptions {
    Bindings::PresentationStyle presentation_style { Bindings::PresentationStyle::Unspecified };
};

// https://w3c.github.io/clipboard-apis/#clipboard-item-interface
class ClipboardItem : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ClipboardItem, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ClipboardItem);

public:
    struct Representation {
        String mime_type;              // The MIME type (e.g., "text/plain").
        bool is_custom { false };      // Whether this is a web custom format.
        GC::Ref<WebIDL::Promise> data; // The actual data for this representation.
    };

    static WebIDL::ExceptionOr<GC::Ref<ClipboardItem>> construct_impl(JS::Realm&, OrderedHashMap<String, GC::Root<WebIDL::Promise>> const& items, ClipboardItemOptions const& options = {});

    virtual ~ClipboardItem() override;

    Bindings::PresentationStyle presentation_style() const { return m_presentation_style; }

    Vector<String> const& types() const { return m_types; }

    Vector<Representation> const& representations() const { return m_representations; }
    void append_representation(Representation);

    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> get_type(String const& type);

    static bool supports(JS::VM&, String const& type);

private:
    ClipboardItem(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    Bindings::PresentationStyle m_presentation_style;
    Vector<String> m_types;
    Vector<Representation> m_representations;
};

}
