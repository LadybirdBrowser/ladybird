/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PerformanceEntry.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::PerformanceTimeline {

enum class AvailableFromTimeline {
    No,
    Yes,
};

enum class ShouldAddEntry {
    No,
    Yes,
};

// https://www.w3.org/TR/performance-timeline/#dom-performanceentry
class PerformanceEntry : public Bindings::Wrappable {
    WEB_WRAPPABLE(PerformanceEntry, Bindings::Wrappable);

public:
    virtual ~PerformanceEntry();

    // https://www.w3.org/TR/performance-timeline/#dom-performanceentry-entrytype
    virtual FlyString const& entry_type() const = 0;

    String const& name() const { return m_name; }
    HighResolutionTime::DOMHighResTimeStamp start_time() const { return m_start_time; }
    HighResolutionTime::DOMHighResTimeStamp duration() const { return m_duration; }

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<Bindings::PerformanceObserverInit const&> = {}) const = 0;

protected:
    PerformanceEntry(String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration);

private:
    // https://www.w3.org/TR/performance-timeline/#dom-performanceentry-name
    String m_name;

    // https://www.w3.org/TR/performance-timeline/#dom-performanceentry-starttime
    HighResolutionTime::DOMHighResTimeStamp m_start_time { 0.0 };

    // https://www.w3.org/TR/performance-timeline/#dom-performanceentry-duration
    HighResolutionTime::DOMHighResTimeStamp m_duration { 0.0 };
};

}
