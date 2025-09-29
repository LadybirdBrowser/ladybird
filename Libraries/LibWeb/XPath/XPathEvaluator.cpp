/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/XPathEvaluatorPrototype.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include "XPathEvaluator.h"
#include "XPathExpression.h"
#include "XPathResult.h"

namespace Web::XPath {

GC_DEFINE_ALLOCATOR(XPathEvaluator);

XPathEvaluator::XPathEvaluator(JS::Realm& realm)
    : Web::Bindings::PlatformObject(realm)
{
}

XPathEvaluator::~XPathEvaluator() = default;

WebIDL::ExceptionOr<GC::Ref<XPathEvaluator>> XPathEvaluator::construct_impl(JS::Realm& realm)
{
    return realm.create<XPathEvaluator>(realm);
}

void XPathEvaluator::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(XPathEvaluator);
    Base::initialize(realm);
}

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> XPathEvaluator::create_expression(String const& expression, GC::Ptr<XPathNSResolver> resolver)
{
    auto& realm = this->realm();
    return realm.create<XPathExpression>(realm, expression, resolver);
}

WebIDL::ExceptionOr<GC::Ref<XPathResult>> XPathEvaluator::evaluate(String const&, DOM::Node const&, GC::Ptr<XPathNSResolver>, WebIDL::UnsignedShort, GC::Ptr<XPathResult>)
{
    auto& realm = this->realm();
    return realm.create<XPathResult>(realm);
}

GC::Ref<DOM::Node> XPathEvaluator::create_ns_resolver(GC::Ref<DOM::Node> node_resolver)
{
    return node_resolver;
}

}
