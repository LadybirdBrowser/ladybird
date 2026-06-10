/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include "XPath.h"
#include "XPathEvaluator.h"
#include "XPathExpression.h"
#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathEvaluator);

XPathEvaluator::XPathEvaluator()
{
}

XPathEvaluator::~XPathEvaluator() = default;

GC::Ref<XPathEvaluator> XPathEvaluator::create()
{
    return GC::Heap::the().allocate<XPathEvaluator>();
}

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> XPathEvaluator::create_expression(String const& expression, GC::Ptr<XPathNSResolver> resolver)
{
    return XPath::create_expression(expression, resolver);
}

WebIDL::ExceptionOr<GC::Ref<XPathResult>> XPathEvaluator::evaluate(String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> resolver, WebIDL::UnsignedShort type, GC::Ptr<XPathResult> result)
{
    return XPath::throw_evaluation_error_if_needed(XPath::evaluate(expression, context_node, resolver, type, result));
}

GC::Ref<DOM::Node> XPathEvaluator::create_ns_resolver(GC::Ref<DOM::Node> node_resolver)
{
    return node_resolver;
}

}
