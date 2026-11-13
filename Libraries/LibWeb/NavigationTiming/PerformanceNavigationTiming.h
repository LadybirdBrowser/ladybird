/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceNavigationTiming.h>
#include <LibWeb/Forward.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::NavigationTiming {

// https://w3c.github.io/navigation-timing/#dom-performancenavigationtiming
class PerformanceNavigationTiming final : public ResourceTiming::PerformanceResourceTiming {
    WEB_RUST_INTERFACE(PerformanceNavigationTiming, performance_navigation_timing)
};

// https://w3c.github.io/navigation-timing/#dfn-create-the-navigation-timing-entry
void create_the_navigation_timing_entry(DOM::Document&, NonnullRefPtr<Fetch::Infrastructure::FetchTimingInfo>, u16 redirect_count, Bindings::NavigationTimingType, Optional<Fetch::Infrastructure::Response::CacheState> const&, Fetch::Infrastructure::Response::BodyInfo const&, Fetch::Infrastructure::Status);

}
