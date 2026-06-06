/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/IntrusiveList.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WEB_API NavigationObserver final : public GC::Cell {
    GC_CELL(NavigationObserver, GC::Cell);
    GC_DECLARE_ALLOCATOR(NavigationObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<NavigationObserver> create(Navigable&);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> navigation_complete() const { return m_navigation_complete; }
    void set_navigation_complete(Function<void()>);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> ongoing_navigation_changed() const { return m_ongoing_navigation_changed; }
    void set_ongoing_navigation_changed(Function<void()>);

private:
    explicit NavigationObserver(Navigable&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual void finalize() override;

    IntrusiveListNode<NavigationObserver> m_list_node;
    GC::Ref<Navigable> m_navigable;
    GC::Ptr<GC::Function<void()>> m_navigation_complete;
    GC::Ptr<GC::Function<void()>> m_ongoing_navigation_changed;

public:
    using NavigationObserversList = IntrusiveList<&NavigationObserver::m_list_node>;
};

}
