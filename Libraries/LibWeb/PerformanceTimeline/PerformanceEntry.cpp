/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PerformanceTimeline {

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

}
