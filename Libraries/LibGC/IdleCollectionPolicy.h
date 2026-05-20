/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace GC {

// Decides, once per idle-timer tick, whether the heap should be proactively collected. See Heap::idle_gc_on_timer().
//
// The GC heap is never completely silent in practice: event-loop housekeeping (queuing an HTML task and so on) keeps a
// small allocation trickle going even on an idle page. So instead of waiting for zero allocation, we watch for the
// mutator transitioning out of an active phase: a tick whose allocation rate has dropped below 1/low_rate_divisor of
// the peak rate seen so far this episode. The rate-drop trigger is gated on enough uncollected garbage having piled up
// to be worth marking the whole live heap. The watchdog bounds how long garbage can sit when the rate-drop trigger
// never fires, e.g. on a heap that allocates too steadily to show a drop or too slowly to clear the gate.
class IdleCollectionPolicy {
public:
    enum class Decision : u8 {
        KeepWaiting, // No collection yet; leave the idle timer running.
        Park,        // Nothing left to collect; stop the timer until allocation resumes.
        Collect,     // Collect now.
    };

    // Begins a fresh episode. `total_allocated_bytes` is the heap's monotonic allocation counter.
    void reset(u64 total_allocated_bytes)
    {
        m_total_allocated_at_last_check = total_allocated_bytes;
        m_peak_delta = 0;
        m_tick_count = 0;
    }

    // Evaluates one idle-timer tick. `total_allocated_bytes` is the monotonic allocation counter, `garbage_bytes` is
    // the amount allocated since the last collection, and `gc_threshold` is the allocation-driven GC threshold.
    Decision evaluate(u64 total_allocated_bytes, size_t garbage_bytes, size_t gc_threshold)
    {
        if (garbage_bytes == 0)
            return Decision::Park;

        auto delta = total_allocated_bytes - m_total_allocated_at_last_check;
        m_total_allocated_at_last_check = total_allocated_bytes;
        m_peak_delta = max(m_peak_delta, delta);

        bool rate_dropped = delta * low_rate_divisor < m_peak_delta;
        bool watchdog_elapsed = ++m_tick_count >= watchdog_ticks;
        bool enough_garbage = garbage_bytes >= gc_threshold / min_garbage_divisor;

        if ((rate_dropped && enough_garbage) || watchdog_elapsed)
            return Decision::Collect;
        return Decision::KeepWaiting;
    }

    // A tick counts as a rate drop when its allocation is below 1/low_rate_divisor of the episode's peak.
    static constexpr u64 low_rate_divisor = 4;
    // The rate-drop trigger only fires once garbage reaches gc_threshold / min_garbage_divisor.
    static constexpr size_t min_garbage_divisor = 16;
    // The watchdog forces a collection after this many ticks regardless of the rate or the gate.
    static constexpr u32 watchdog_ticks = 15;

private:
    u64 m_total_allocated_at_last_check { 0 };
    u64 m_peak_delta { 0 };
    u32 m_tick_count { 0 };
};

}
