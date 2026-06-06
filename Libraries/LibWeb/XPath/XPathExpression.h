/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/XPathExpression.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/XPath/XPathNSResolver.h>
#include <LibWeb/XPath/XPathResult.h>

namespace Web::XPath {

class XPathExpression final : public Bindings::Wrappable {
    WEB_WRAPPABLE(XPathExpression, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(XPathExpression);

public:
    [[nodiscard]] static GC::Ref<XPathExpression> create(String const& expression, GC::Ptr<XPathNSResolver> resolver);

    explicit XPathExpression(String const& expression, GC::Ptr<XPathNSResolver> resolver);
    virtual ~XPathExpression() override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(JS::Realm&, DOM::Node const& context_node, WebIDL::UnsignedShort type = 0, GC::Ptr<XPathResult> result = nullptr);

private:
    String m_expression;
    GC::Ptr<XPathNSResolver> m_resolver;
};

}
