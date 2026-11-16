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

GC::Ref<XPathExpression> XPathExpression::create(NonnullOwnPtr<Expression> expression)
{
    return GC::Heap::the().allocate<XPathExpression>(move(expression));
}

XPathExpression::XPathExpression(NonnullOwnPtr<Expression> expression)
    : m_expression(move(expression))
{
}

XPathExpression::~XPathExpression() = default;

WebIDL::ExceptionOr<GC::Ref<XPathResult>> XPathExpression::evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result)
{
    return m_expression->evaluate(context_node, type, result);
}

}
