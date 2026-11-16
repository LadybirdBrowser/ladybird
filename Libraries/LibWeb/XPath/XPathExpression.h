/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/XPath/XPathNSResolver.h>
#include <LibWeb/XPath/XPathResult.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::XPath {

class Expression;

class XPathExpression final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(XPathExpression, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(XPathExpression);

public:
    [[nodiscard]] static GC::Ref<XPathExpression> create(NonnullOwnPtr<Expression>);

    explicit XPathExpression(NonnullOwnPtr<Expression>);
    virtual ~XPathExpression() override;

    WebIDL::ExceptionOr<GC::Ref<XPathResult>> evaluate(DOM::Node const& context_node, WebIDL::UnsignedShort type = 0, GC::Ptr<XPathResult> result = nullptr);

private:
    NonnullOwnPtr<Expression> m_expression;
};

}
