/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WEB_API NavigationObserver final : public Bindings::PlatformObject {
    WEB_NON_IDL_PLATFORM_OBJECT(NavigationObserver, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(NavigationObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] GC::Ptr<GC::Function<void()>> navigation_complete() const { return m_navigation_complete; }
    void set_navigation_complete(Function<void()>);

    [[nodiscard]] GC::Ptr<GC::Function<void()>> ongoing_navigation_changed() const { return m_ongoing_navigation_changed; }
    void set_ongoing_navigation_changed(Function<void()>);

private:
    NavigationObserver(JS::Realm&, Navigable&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    IntrusiveListNode<NavigationObserver> m_list_node;
    GC::Ref<Navigable> m_navigable;
    GC::Ptr<GC::Function<void()>> m_navigation_complete;
    GC::Ptr<GC::Function<void()>> m_ongoing_navigation_changed;

public:
    using NavigationObserversList = IntrusiveList<&NavigationObserver::m_list_node>;
};

}
