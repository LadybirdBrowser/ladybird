/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>
#include <LibWebCommon/HTML/VisibilityState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry
class VisibilityStateEntry final : public PerformanceTimeline::PerformanceEntry {
    WEB_WRAPPABLE(VisibilityStateEntry, PerformanceTimeline::PerformanceEntry);
    GC_DECLARE_ALLOCATOR(VisibilityStateEntry);

public:
    virtual ~VisibilityStateEntry() override;

    static GC::Ref<VisibilityStateEntry> create(VisibilityState, HighResolutionTime::DOMHighResTimeStamp timestamp);

    // https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
    virtual PerformanceTimeline::ShouldAddEntry should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&> = {}) const override { return PerformanceTimeline::ShouldAddEntry::Yes; }

    virtual Utf16FlyString const& entry_type() const override;

private:
    VisibilityStateEntry(VisibilityState, HighResolutionTime::DOMHighResTimeStamp timestamp);
};

}
