/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/XPathEvaluator.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>

#include "XPathExpression.h"
#include "XPathNSResolver.h"
#include "XPathResult.h"

namespace Web::XPath {

class XPathEvaluator : public Bindings::Wrappable {
    WEB_WRAPPABLE(XPathEvaluator, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(XPathEvaluator);

    explicit XPathEvaluator();
    virtual ~XPathEvaluator() override;

public:
    static WebIDL::ExceptionOr<GC::Ref<XPathEvaluator>> construct_impl();
    [[nodiscard]] static GC::Ref<XPathEvaluator> create();

    WebIDL::ExceptionOr<GC::Ref<XPathExpression>> create_expression(JS::Realm&, String const& expression, GC::Ptr<XPathNSResolver> resolver = nullptr);
    WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(JS::Realm&, String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> resolver = nullptr, WebIDL::UnsignedShort type = 0, GC::Ptr<XPathResult> result = nullptr);
    static GC::Ref<DOM::Node> create_ns_resolver(GC::Ref<DOM::Node> node_resolver); // legacy
};

}
