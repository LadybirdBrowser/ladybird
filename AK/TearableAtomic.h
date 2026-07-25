/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/BitCast.h>
#include <AK/Noncopyable.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>

namespace AK {

// Storage for a trivially-copyable value that is read and written through word-sized atomic accesses, so that
// concurrent loads and stores of the same value are not data races. The value can still tear on word boundaries, so
// data coherence is up to the caller to guarantee.
template<typename T>
class TearableAtomic {
    static_assert(IsTriviallyCopyable<T>, "TearableAtomic requires a trivially-copyable value type");

    AK_MAKE_NONCOPYABLE(TearableAtomic);
    AK_MAKE_NONMOVABLE(TearableAtomic);

public:
    TearableAtomic() = default;

    T load_relaxed() const
    {
        alignas(T) u8 bytes[sizeof(T)];
        size_t offset = 0;
        for (; offset + sizeof(u64) <= sizeof(T); offset += sizeof(u64)) {
            u64 word = atomic_load(reinterpret_cast<u64 volatile*>(const_cast<u8*>(m_storage) + offset), MemoryOrder::memory_order_relaxed);
            __builtin_memcpy(bytes + offset, &word, sizeof(word));
        }
        for (; offset < sizeof(T); offset++)
            bytes[offset] = atomic_load(reinterpret_cast<u8 volatile*>(const_cast<u8*>(m_storage) + offset), MemoryOrder::memory_order_relaxed);
        return bit_cast<T>(bytes);
    }

    void store_relaxed(T const& value)
    {
        auto const* bytes = reinterpret_cast<u8 const*>(&value);
        size_t offset = 0;
        for (; offset + sizeof(u64) <= sizeof(T); offset += sizeof(u64)) {
            u64 word;
            __builtin_memcpy(&word, bytes + offset, sizeof(word));
            atomic_store(reinterpret_cast<u64 volatile*>(m_storage + offset), word, MemoryOrder::memory_order_relaxed);
        }
        for (; offset < sizeof(T); offset++)
            atomic_store(reinterpret_cast<u8 volatile*>(m_storage + offset), bytes[offset], MemoryOrder::memory_order_relaxed);
    }

private:
    alignas(T) alignas(u64) u8 m_storage[sizeof(T)] {};
};

}

#if USING_AK_GLOBALLY
using AK::TearableAtomic;
#endif
