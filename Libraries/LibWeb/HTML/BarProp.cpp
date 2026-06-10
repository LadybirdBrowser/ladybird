/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BarProp.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BarProp);

GC::Ref<BarProp> BarProp::create(Window& window)
{
    return GC::Heap::the().allocate<BarProp>(window);
}

BarProp::BarProp(Window& window)
    : m_window(window)
{
}

void BarProp::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-barprop-visible
bool BarProp::visible() const
{
    // 1. Let browsingContext be this's relevant global object's browsing context.
    auto browsing_context = m_window->associated_document().browsing_context();

    // 2. If browsingContext is null, then return true.
    if (!browsing_context) {
        return true;
    }

    // 3. Return the negation of browsingContext's top-level browsing context's is popup.
    auto top_level_browsing_context = browsing_context->top_level_browsing_context();
    if (!top_level_browsing_context)
        return true;
    return top_level_browsing_context->is_popup() != TokenizedFeature::Popup::Yes;
}

}
