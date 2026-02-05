/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

#include "XPathExpression.h"
#include "XPathNSResolver.h"
#include "XPathResult.h"

namespace Web::XPath {

WebIDL::ExceptionOr<GC::Ref<XPathExpression>> create_expression(JS::Realm& realm, String const& expression, GC::Ptr<XPathNSResolver> resolver);
WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(JS::Realm& realm, String const& expression, DOM::Node const& context_node, GC::Ptr<XPathNSResolver> resolver, unsigned short type, GC::Ptr<XPathResult> result);

}
