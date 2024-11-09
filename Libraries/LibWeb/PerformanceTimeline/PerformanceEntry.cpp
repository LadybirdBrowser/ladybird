/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceEntryPrototype.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PerformanceTimeline {

PerformanceEntry::PerformanceEntry(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration)
    : Bindings::PlatformObject(realm)
    , m_name(name)
    , m_start_time(start_time)
    , m_duration(duration)
{
}

PerformanceEntry::~PerformanceEntry() = default;

void PerformanceEntry::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceEntry);
}

}
