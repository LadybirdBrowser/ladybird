/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PerformanceTimeline {

extern "C" {
FlatPtr web_performance_timeline_entry_name(PerformanceEntry*);
FlatPtr web_performance_timeline_entry_entry_type(PerformanceEntry*);
double web_performance_timeline_entry_start_time(PerformanceEntry*);
bool web_performance_timeline_entry_should_add_entry(PerformanceEntry*, bool has_options, bool has_duration_threshold, double duration_threshold);
}

PerformanceEntry::PerformanceEntry(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration)
    : m_name(Utf16String::from_utf8(name))
    , m_start_time(start_time)
    , m_duration(duration)
{
}

PerformanceEntry::PerformanceEntry(Utf16String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration)
    : m_name(name)
    , m_start_time(start_time)
    , m_duration(duration)
{
}

PerformanceEntry::~PerformanceEntry() = default;

extern "C" {

FlatPtr web_performance_timeline_entry_name(PerformanceEntry* entry)
{
    return Utf16String { entry->name() }.into_raw();
}

FlatPtr web_performance_timeline_entry_entry_type(PerformanceEntry* entry)
{
    return entry->entry_type().to_utf16_string().into_raw();
}

double web_performance_timeline_entry_start_time(PerformanceEntry* entry)
{
    return entry->start_time();
}

bool web_performance_timeline_entry_should_add_entry(PerformanceEntry* entry, bool has_options, bool has_duration_threshold, double duration_threshold)
{
    Optional<PerformanceObserverInit> options;
    if (has_options) {
        options = PerformanceObserverInit {};
        if (has_duration_threshold)
            options->duration_threshold = duration_threshold;
    }
    if (options.has_value())
        return entry->should_add_entry(*options) == ShouldAddEntry::Yes;
    return entry->should_add_entry() == ShouldAddEntry::Yes;
}
}

}
