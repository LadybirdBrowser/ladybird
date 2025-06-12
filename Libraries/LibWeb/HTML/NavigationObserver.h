/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class NavigationObserver final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NavigationObserver, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(NavigationObserver);

public:
    [[nodiscard]] GC::Ptr<GC::Function<void()>> navigation_complete() const { return m_navigation_complete; }
    void set_navigation_complete(Function<void()>);

private:
    NavigationObserver(JS::Realm&, Navigable&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    GC::Ref<Navigable> m_navigable;
    GC::Ptr<GC::Function<void()>> m_navigation_complete;
};

}
