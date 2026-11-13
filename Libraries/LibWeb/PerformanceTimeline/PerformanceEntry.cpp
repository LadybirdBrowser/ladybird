/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PerformanceTimeline {

extern "C" {
// Defined in Bindings/Rust/src/performance_timeline/performance_entry.rs.
FlatPtr web_rust_performance_entry_name(PerformanceEntry const*);
double web_rust_performance_entry_start_time(PerformanceEntry const*);
double web_rust_performance_entry_duration(PerformanceEntry const*);
}

Utf16String PerformanceEntry::name() const
{
    return Utf16String::adopt_raw(web_rust_performance_entry_name(this));
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEntry::start_time() const
{
    return web_rust_performance_entry_start_time(this);
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEntry::duration() const
{
    return web_rust_performance_entry_duration(this);
}

}
