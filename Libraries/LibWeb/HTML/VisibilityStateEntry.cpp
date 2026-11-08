/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Document.h>
#include <LibWeb/HTML/VisibilityStateEntry.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(VisibilityStateEntry);

// https://html.spec.whatwg.org/multipage/interaction.html#visibilitystateentry
VisibilityStateEntry::VisibilityStateEntry(VisibilityState visibility_state, HighResolutionTime::DOMHighResTimeStamp timestamp)
    // The name getter steps are to return this's visibility state.
    // The startTime getter steps are to return this's timestamp.
    // The duration getter steps are to return zero.
    : PerformanceTimeline::PerformanceEntry(Bindings::idl_enum_to_string(visibility_state), timestamp, 0)
{
}

VisibilityStateEntry::~VisibilityStateEntry() = default;

GC::Ref<VisibilityStateEntry> VisibilityStateEntry::create(VisibilityState visibility_state, HighResolutionTime::DOMHighResTimeStamp timestamp)
{
    return GC::Heap::the().allocate<VisibilityStateEntry>(visibility_state, timestamp);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-visibilitystateentry-entrytype
Utf16FlyString const& VisibilityStateEntry::entry_type() const
{
    // The entryType getter steps are to return "visibility-state".
    return PerformanceTimeline::EntryTypes::visibility_state;
}

}
