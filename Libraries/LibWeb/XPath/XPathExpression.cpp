/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Forward.h>

#include "XPath.h"
#include "XPathEvaluator.h"
#include "XPathExpression.h"
#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathExpression);

GC::Ref<XPathExpression> XPathExpression::create(String const& expression, GC::Ptr<XPathNSResolver> resolver)
{
    return GC::Heap::the().allocate<XPathExpression>(expression, resolver);
}

XPathExpression::XPathExpression(String const& expression, GC::Ptr<XPathNSResolver> resolver)
    : m_expression(expression)
    , m_resolver(resolver)
{
}

void XPathExpression::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_resolver);
}

XPathExpression::~XPathExpression() = default;

WebIDL::ExceptionOr<GC::Ref<XPathResult>> XPathExpression::evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result)
{
    return XPath::throw_evaluation_error_if_needed(XPath::evaluate(m_expression, context_node, m_resolver, type, result));
}

}
