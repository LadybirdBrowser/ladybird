/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#xmlserializer
class XMLSerializer final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(XMLSerializer, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(XMLSerializer);

public:
    static GC::Ref<XMLSerializer> create();

    virtual ~XMLSerializer() override;

    WebIDL::ExceptionOr<Utf16String> serialize_to_string(GC::Ref<DOM::Node const> root);

private:
    XMLSerializer();
};

enum class RequireWellFormed {
    No,
    Yes,
};

WebIDL::ExceptionOr<Utf16String> serialize_node_to_xml_string(GC::Ref<DOM::Node const> root, RequireWellFormed require_well_formed);

}
