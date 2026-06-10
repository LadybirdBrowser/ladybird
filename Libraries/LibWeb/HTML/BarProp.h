/*
 * Copyright (c) 2025, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#barprop
class BarProp : public Bindings::Wrappable {
    WEB_WRAPPABLE(BarProp, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(BarProp);

public:
    BarProp(Window&);
    static GC::Ref<BarProp> create(Window&);

    [[nodiscard]] Window& window() { return m_window; }
    [[nodiscard]] Window const& window() const { return m_window; }
    [[nodiscard]] bool visible() const;

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<Window> m_window;
};

}
