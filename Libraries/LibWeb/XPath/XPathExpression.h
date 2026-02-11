/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/XPath/XPathNSResolver.h>
#include <LibWeb/XPath/XPathResult.h>

namespace Web::XPath {

class XPathExpression final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(XPathExpression, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(XPathExpression);

public:
    explicit XPathExpression(JS::Realm&, String const& expression, GC::Ptr<XPathNSResolver> resolver);
    virtual ~XPathExpression() override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void initialize(JS::Realm&) override;

    WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type = 0, GC::Ptr<XPathResult> result = nullptr);

private:
    String m_expression;
    GC::Ptr<XPathNSResolver> m_resolver;
};

}
