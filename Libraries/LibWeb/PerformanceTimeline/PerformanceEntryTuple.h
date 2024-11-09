/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::PerformanceTimeline {

// https://www.w3.org/TR/performance-timeline/#dfn-performance-entry-buffer-map
struct PerformanceEntryTuple {
    // https://www.w3.org/TR/performance-timeline/#dfn-performance-entry-buffer
    // A performance entry buffer to store PerformanceEntry objects, that is initially empty.
    Vector<JS::NonnullGCPtr<PerformanceEntry>> performance_entry_buffer;

    // https://www.w3.org/TR/performance-timeline/#dfn-maxbuffersize
    // An integer maxBufferSize, initialized to the registry value for this entry type.
    // NOTE: The empty state represents Infinite size.
    Optional<u64> max_buffer_size;

    // https://www.w3.org/TR/performance-timeline/#dfn-availablefromtimeline
    // A boolean availableFromTimeline, initialized to the registry value for this entry type.
    AvailableFromTimeline available_from_timeline { AvailableFromTimeline::No };

    // https://www.w3.org/TR/performance-timeline/#dfn-dropped-entries-count
    // An integer dropped entries count that is initially 0.
    u64 dropped_entries_count { 0 };

    // https://www.w3.org/TR/performance-timeline/#dfn-determine-if-a-performance-entry-buffer-is-full
    bool is_full()
    {
        // 1. Let num current entries be the size of tuple's performance entry buffer.
        auto num_current_entries = performance_entry_buffer.size();

        // 2. If num current entries is less than tuples's maxBufferSize, return false.
        if (!max_buffer_size.has_value() || num_current_entries < max_buffer_size.value())
            return false;

        // 3. Increase tuple's dropped entries count by 1.
        ++dropped_entries_count;

        // 4. Return true.
        return true;
    }

    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(performance_entry_buffer);
    }
};

}
