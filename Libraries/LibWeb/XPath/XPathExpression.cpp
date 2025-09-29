/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/XPathExpressionPrototype.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>

#include "XPath.h"
#include "XPathEvaluator.h"
#include "XPathExpression.h"
#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathExpression);

XPathExpression::XPathExpression(JS::Realm& realm, String const& expression, GC::Ptr<XPathNSResolver> resolver)
    : Web::Bindings::PlatformObject(realm)
    , m_expression(expression)
    , m_resolver(resolver)
{
}

void XPathExpression::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XPathExpression);
    Base::initialize(realm);
}

void XPathExpression::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_resolver);
}

XPathExpression::~XPathExpression() = default;

WebIDL::ExceptionOr<GC::Ref<XPathResult>> XPathExpression::evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result)
{
    auto& realm = this->realm();
    return XPath::evaluate(realm, m_expression, context_node, m_resolver, type, result);
}

}
