/*
 * Copyright (c) 2025, Johannes Gustafsson <johannesgu@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/CallbackType.h>

namespace Web::XPath {

class XPathNSResolver final : public GC::Cell {
    GC_CELL(XPathNSResolver, GC::Cell);
    GC_DECLARE_ALLOCATOR(XPathNSResolver);

public:
    [[nodiscard]] static GC::Ref<XPathNSResolver> create(GC::Ref<WebIDL::CallbackType>);

    virtual ~XPathNSResolver() = default;

    WebIDL::CallbackType& callback() { return *m_callback; }

private:
    explicit XPathNSResolver(GC::Ref<WebIDL::CallbackType>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<WebIDL::CallbackType> m_callback;
};

}
