/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#xmlserializer
class XMLSerializer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(XMLSerializer, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(XMLSerializer);

public:
    static WebIDL::ExceptionOr<GC::Ref<XMLSerializer>> construct_impl(JS::Realm&);

    virtual ~XMLSerializer() override;

    WebIDL::ExceptionOr<String> serialize_to_string(GC::Ref<DOM::Node const> root);

private:
    explicit XMLSerializer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

enum class RequireWellFormed {
    No,
    Yes,
};

WebIDL::ExceptionOr<String> serialize_node_to_xml_string(GC::Ref<DOM::Node const> root, RequireWellFormed require_well_formed);
}
