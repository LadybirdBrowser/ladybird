/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

#include "XPathResult.h"

namespace Web::XPath {

class XPathNSResolver final : public JS::Object {
    JS_OBJECT(XPathNSResolver, JS::Object);
    GC_DECLARE_ALLOCATOR(XPathNSResolver);

public:
    [[nodiscard]] static GC::Ref<XPathNSResolver> create(JS::Realm&, GC::Ref<WebIDL::CallbackType>);
    XPathNSResolver(JS::Realm&, GC::Ref<WebIDL::CallbackType>);

    virtual ~XPathNSResolver() = default;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<WebIDL::CallbackType> m_callback;
};

}
