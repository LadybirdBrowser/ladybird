/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
#include <AK/TearableAtomic.h>
#include <AK/Types.h>

namespace AK {

// A sequence lock: a single writer publishes data that multiple readers can snapshot without blocking other readers
// and the writer. Reads are guaranteed to be tear-free, but may fail to read if a write doesn't complete before a
// certain number of retries.
//
// The value is accessed through TearableAtomic, so reads racing a write are defined behavior and discarded by the
// version check rather than relying on implementation-specific handling of racing plain accesses.
template<typename T>
class SeqLock {
    AK_MAKE_NONCOPYABLE(SeqLock);
    AK_MAKE_NONMOVABLE(SeqLock);

    static_assert(IsTriviallyCopyable<T>, "SeqLock requires a trivially-copyable value type");

public:
    SeqLock() = default;

    static constexpr size_t MAX_READ_RETRIES = 32;

    template<typename Callback>
    void write(Callback&& callback)
    {
        auto version = m_version.load(AK::MemoryOrder::memory_order_relaxed);
        m_version.store(version + 1, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_thread_fence(AK::MemoryOrder::memory_order_seq_cst);
        callback(m_value);
        m_version.store(version + 2, AK::MemoryOrder::memory_order_release);
    }

    void store(T const& value)
    {
        write([&](TearableAtomic<T>& slot) { slot.store_relaxed(value); });
    }

    // Returns an empty Optional if a stable snapshot could not be captured within MAX_READ_RETRIES.
    Optional<T> read() const
    {
        for (size_t retry = 0; retry < MAX_READ_RETRIES; retry++) {
            auto version_before = m_version.load(AK::MemoryOrder::memory_order_acquire);
            if (version_before % 2 != 0)
                continue;

            T copy = m_value.load_relaxed();

            AK::atomic_thread_fence(AK::MemoryOrder::memory_order_acquire);
            auto version_after = m_version.load(AK::MemoryOrder::memory_order_relaxed);
            if (version_before == version_after)
                return copy;
        }
        return {};
    }

    // Reads without the sequence protocol; only safe on the writer thread.
    T weak_read() const { return m_value.load_relaxed(); }

private:
    Atomic<u64> m_version { 0 };
    TearableAtomic<T> m_value;
};

}

#if USING_AK_GLOBALLY
using AK::SeqLock;
#endif
