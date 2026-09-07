/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>

namespace Web::NavigationTiming {

// https://w3c.github.io/navigation-timing/#the-performancenavigation-interface
class PerformanceNavigation final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(PerformanceNavigation, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(PerformanceNavigation);

public:
    static constexpr u16 TYPE_NAVIGATE = 0;
    static constexpr u16 TYPE_RELOAD = 1;
    static constexpr u16 TYPE_BACK_FORWARD = 2;
    static constexpr u16 TYPE_RESERVED = 255;

    static GC::Ref<PerformanceNavigation> create(HTML::Window&);

    ~PerformanceNavigation();

    u16 type() const;
    u16 redirect_count() const;

private:
    explicit PerformanceNavigation(HTML::Window&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<PerformanceNavigationTiming> navigation_timing_entry() const;

    GC::Ref<HTML::Window> m_window;
};

}
