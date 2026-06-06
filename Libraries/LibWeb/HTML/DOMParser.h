/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/TrustedTypes/TrustedHTML.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#domparser
class DOMParser final : public Bindings::Wrappable {
    WEB_WRAPPABLE(DOMParser, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(DOMParser);

public:
    static WebIDL::ExceptionOr<GC::Ref<DOMParser>> construct_impl(JS::Realm&);

    virtual ~DOMParser() override;

    WebIDL::ExceptionOr<GC::Ref<DOM::Document>> parse_from_string(TrustedTypes::TrustedHTMLOrString, Bindings::DOMParserSupportedType type);

private:
    explicit DOMParser(JS::Realm&);
};

}
