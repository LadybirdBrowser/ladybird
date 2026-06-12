/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/TearableAtomic.h>
#include <LibMedia/AudioBlockTiming.h>

namespace Media {

// Written by a single writer; readers are lock-free and may access the ring in place from
// other threads or, through shared memory, other processes.
class AudioBlockTimingRing {
    AK_MAKE_NONCOPYABLE(AudioBlockTimingRing);
    AK_MAKE_NONMOVABLE(AudioBlockTimingRing);

public:
    AudioBlockTimingRing() = default;

    static constexpr size_t capacity = 32;

    void clear()
    {
        m_first_valid_sequence = m_latest_sequence.load() + 1;
    }

    void enqueue(AudioBlockTiming timing)
    {
        auto sequence = m_latest_sequence.load() + 1;
        auto& slot = m_slots[sequence % capacity];

        auto version = slot.version.load();
        slot.version.store(version + 1);
        AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);
        slot.record.store_relaxed({ .sequence = sequence, .timing = timing });
        slot.version.store(version + 2, AK::MemoryOrder::memory_order_release);

        m_latest_sequence.store(sequence);
    }

    Optional<AudioBlockTiming> latest_timing() const
    {
        auto latest_sequence = m_latest_sequence.load();
        if (latest_sequence < m_first_valid_sequence.load())
            return {};

        auto record = read_record(latest_sequence);
        if (!record.has_value())
            return {};
        return record->timing;
    }

    Optional<AudioBlockTiming> find_timing_for_frame_index(i64 frame_index) const
    {
        auto latest_sequence = m_latest_sequence.load();
        auto first_valid_sequence = m_first_valid_sequence.load();
        if (latest_sequence < first_valid_sequence)
            return {};

        Optional<AudioBlockTiming> oldest_timing;
        auto sequence_count = min<u64>(latest_sequence - first_valid_sequence + 1, capacity);
        for (u64 i = 0; i < sequence_count; ++i) {
            auto sequence = latest_sequence - i;
            if (sequence < first_valid_sequence)
                break;

            auto record = read_record(sequence);
            if (!record.has_value())
                continue;

            auto const& timing = record->timing;
            if (timing.contains_frame_index(frame_index))
                return timing;
            if (frame_index >= timing.end_frame_index())
                return timing;
            oldest_timing = timing;
        }

        return oldest_timing;
    }

private:
    struct Record {
        u64 sequence { 0 };
        AudioBlockTiming timing;
    };

    // Ordering between the version and the payload is provided by the fences in enqueue() and
    // read_record(), and by their explicitly annotated version accesses.
    struct Slot {
        Atomic<u64, AK::MemoryOrder::memory_order_relaxed> version { 0 };
        TearableAtomic<Record> record;
    };

    Optional<Record> read_record(u64 expected_sequence) const
    {
        auto const& slot = m_slots[expected_sequence % capacity];

        auto version_before = slot.version.load(AK::MemoryOrder::memory_order_acquire);
        if (version_before % 2 != 0)
            return {};

        auto record = slot.record.load_relaxed();

        AK::atomic_thread_fence(AK::MemoryOrder::memory_order_acquire);
        auto version_after = slot.version.load();
        if (version_before != version_after || version_after % 2 != 0)
            return {};
        if (record.sequence != expected_sequence)
            return {};
        return record;
    }

    Atomic<u64> m_latest_sequence { 0 };
    Atomic<u64> m_first_valid_sequence { 1 };
    Array<Slot, capacity> m_slots;
};

}
