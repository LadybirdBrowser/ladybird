/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::DOMParsing {

class XMLSerializer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(XMLSerializer, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(XMLSerializer);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<XMLSerializer>> construct_impl(JS::Realm&);

    virtual ~XMLSerializer() override;

    WebIDL::ExceptionOr<String> serialize_to_string(JS::NonnullGCPtr<DOM::Node const> root);

private:
    explicit XMLSerializer(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

enum class RequireWellFormed {
    No,
    Yes,
};

WebIDL::ExceptionOr<String> serialize_node_to_xml_string(JS::NonnullGCPtr<DOM::Node const> root, RequireWellFormed require_well_formed);
}
