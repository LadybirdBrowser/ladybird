/*
 * Copyright (c) 2024, Colin Reeder <colin@vpzom.click>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceNavigation.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::NavigationTiming {

class PerformanceNavigation final : public Bindings::Wrappable {
    WEB_WRAPPABLE(PerformanceNavigation, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PerformanceNavigation);

public:
    static GC::Ref<PerformanceNavigation> create(u16 type, u16 redirect_count);

    ~PerformanceNavigation();

    u16 type() const;
    u16 redirect_count() const;

private:
    PerformanceNavigation(u16 type, u16 redirect_count);

    u16 m_type;
    u16 m_redirect_count;
};

}
