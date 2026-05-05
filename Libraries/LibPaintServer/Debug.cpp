/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <LibPaintServer/Debug.h>

namespace PaintServer {

u32 dbglog_occurrence_count(StringView key)
{
    static HashMap<ByteString, u32> s_counts;

    auto owned_key = ByteString { key };
    auto it = s_counts.find(owned_key);
    if (it == s_counts.end()) {
        s_counts.set(move(owned_key), 1);
        return 0;
    }

    return it->value++;
}

bool dbg_has_elapsed(StringView key, u32 interval_ms)
{
    static HashMap<ByteString, u64> s_last_log_times_ms;

    ByteString const owned_key { key };
    u64 const now_ms = static_cast<u64>(MonotonicTime::now().milliseconds());

    auto it = s_last_log_times_ms.find(owned_key);
    if (it == s_last_log_times_ms.end()) {
        s_last_log_times_ms.set(owned_key, now_ms);
        return true;
    }

    if (now_ms < it->value || now_ms - it->value >= interval_ms) {
        it->value = now_ms;
        return true;
    }

    return false;
}

void dbgtrack(StringView key, float value, Optional<u32> interval_ms)
{
    struct TrackEntry {
        u64 interval_ms { 1000 };
        u64 last_report_time_ms { 0 };
        u32 sample_count { 0 };
        f32 sum { 0.f };
        f32 min { 0.f };
        f32 max { 0.f };
    };
    static HashMap<ByteString, TrackEntry> s_entries;
    ByteString const owned_key { key };
    u64 const now_ms = static_cast<u64>(MonotonicTime::now().milliseconds());

    auto it = s_entries.find(owned_key);
    if (it == s_entries.end()) {
        s_entries.set(owned_key, TrackEntry { .last_report_time_ms = now_ms });
        it = s_entries.find(owned_key);
    }
    auto& entry = it->value;
    if (interval_ms.has_value())
        entry.interval_ms = interval_ms.value();

    if (entry.sample_count == 0) {
        entry.sample_count = 1;
        entry.sum = value;
        entry.min = value;
        entry.max = value;
    } else {
        ++entry.sample_count;
        entry.sum += value;
        entry.min = min(entry.min, value);
        entry.max = max(entry.max, value);
    }

    if (now_ms < entry.last_report_time_ms || now_ms - entry.last_report_time_ms < entry.interval_ms)
        return;

    u32 const sample_count = entry.sample_count;
    f32 const average = entry.sum / static_cast<f32>(sample_count);
    dbgln("{}: avg={} min={} max={} sum={} samples={}", key, average, entry.min, entry.max, entry.sum, sample_count);

    entry.last_report_time_ms = now_ms;
    entry.sample_count = 0;
    entry.sum = 0.f;
    entry.min = 0.f;
    entry.max = 0.f;
}

}
