/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Ptr.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>

namespace Web::IndexedDB {

struct RecordRange {
    size_t start { 0 };
    size_t end { 0 };
};

template<typename Records>
static size_t first_record_index_with_key_at_or_after(Records const& records, GC::Ref<Key> key, bool open)
{
    size_t low = 0;
    size_t high = records.size();

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        auto comparison = Key::compare_two_keys(records[middle].key, key);
        if (comparison < 0 || (comparison == 0 && open))
            low = middle + 1;
        else
            high = middle;
    }

    return low;
}

template<typename Records>
static size_t first_record_index_with_key_after(Records const& records, GC::Ref<Key> key, bool open, size_t start = 0)
{
    size_t low = start;
    size_t high = records.size();

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        auto comparison = Key::compare_two_keys(records[middle].key, key);
        if (comparison < 0 || (comparison == 0 && !open))
            low = middle + 1;
        else
            high = middle;
    }

    return low;
}

template<typename Records>
static RecordRange record_range_for_key_range(Records const& records, GC::Ref<IDBKeyRange> range)
{
    size_t start = 0;
    size_t end = records.size();

    if (auto lower = range->lower_key())
        start = first_record_index_with_key_at_or_after(records, *lower, range->lower_open());

    if (auto upper = range->upper_key())
        end = first_record_index_with_key_after(records, *upper, range->upper_open(), start);

    return { start, end };
}

}
