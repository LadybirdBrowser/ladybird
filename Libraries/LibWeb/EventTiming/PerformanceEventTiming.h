/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceEventTiming.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::EventTiming {

// https://w3c.github.io/event-timing/#sec-performance-event-timing
class PerformanceEventTiming final : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(PerformanceEventTiming, performance_event_timing)
};

}
