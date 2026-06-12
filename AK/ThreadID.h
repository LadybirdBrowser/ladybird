/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Types.h>

namespace AK {

// Identifies a thread for the lifetime of that thread; the OS may reuse IDs after a thread
// exits. A default-constructed ThreadID identifies no thread.
class ThreadID {
public:
    static ThreadID current();

    constexpr ThreadID() = default;

    constexpr bool is_valid() const { return m_id != 0; }
    bool is_current_thread() const { return is_valid() && *this == current(); }

    constexpr bool operator==(ThreadID const&) const = default;

    constexpr u64 value() const { return m_id; }

private:
    constexpr explicit ThreadID(u64 id)
        : m_id(id)
    {
    }

    u64 m_id { 0 };
};

template<>
struct Formatter<ThreadID> : Formatter<u64> {
    ErrorOr<void> format(FormatBuilder& builder, ThreadID thread_id)
    {
        return Formatter<u64>::format(builder, thread_id.value());
    }
};

}

// Deliberately not exported to the global namespace: macOS CarbonCore declares a global
// ThreadID typedef.
