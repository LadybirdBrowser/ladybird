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
// to be worth marking the whole live heap. A tick that allocates next to nothing is a quiet one, which collects once
// a few MiB of garbage are there, however large the heap and its threshold: nobody waits for a quiet heap, and what
// the garbage holds goes back to the system long before the threshold would have come. The watchdog bounds how long
// garbage can sit when neither trigger fires, e.g. on a heap that allocates too steadily to show a drop or too
// slowly to clear the gates.
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
        m_last_tick_was_quiet = false;
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
        m_last_tick_was_quiet = delta <= quiet_tick_bytes;
        bool watchdog_elapsed = ++m_tick_count >= watchdog_ticks;
        bool enough_garbage = garbage_bytes >= gc_threshold / min_garbage_divisor;
        bool enough_garbage_for_a_quiet_heap = garbage_bytes >= quiet_min_garbage_bytes;

        if ((rate_dropped && enough_garbage) || (m_last_tick_was_quiet && enough_garbage_for_a_quiet_heap) || watchdog_elapsed)
            return Decision::Collect;
        return Decision::KeepWaiting;
    }

    // Whether the last tick that was evaluated allocated next to nothing.
    bool last_tick_was_quiet() const { return m_last_tick_was_quiet; }

    // A tick counts as a rate drop when its allocation is below 1/low_rate_divisor of the episode's peak.
    static constexpr u64 low_rate_divisor = 4;
    // The rate-drop trigger only fires once garbage reaches gc_threshold / min_garbage_divisor.
    static constexpr size_t min_garbage_divisor = 16;
    // A tick that allocates at most this much is a quiet one, and a quiet tick collects once this much is garbage.
    static constexpr u64 quiet_tick_bytes = 1 * MiB;
    static constexpr size_t quiet_min_garbage_bytes = 2 * MiB;
    // The watchdog forces a collection after this many ticks regardless of the rate or the gate.
    static constexpr u32 watchdog_ticks = 15;

private:
    u64 m_total_allocated_at_last_check { 0 };
    u64 m_peak_delta { 0 };
    u32 m_tick_count { 0 };
    bool m_last_tick_was_quiet { false };
};

}
