/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalsBase.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalsBase);

InternalsBase::InternalsBase(HTML::Window& window)
    : m_window(window)
{
}

InternalsBase::~InternalsBase() = default;

void InternalsBase::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

HTML::Window& InternalsBase::window() const
{
    return *m_window;
}

Page& InternalsBase::page() const
{
    return window().page();
}

}
