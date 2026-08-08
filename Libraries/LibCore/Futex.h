/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/Export.h>

namespace Core {

enum class AtomicWaitResult : u8 {
    Woken,
    NotEqual,
    TimedOut,
};

// Compare-and-wait on a shared-memory word (size is 4 or 8 bytes). If the value at address doesn't equal expected,
// returns NotEqual immediately; otherwise, blocks until woken by atomic_notify on the same address, or the (optional)
// timeout elapses. For cross-agent coordination the address must live in memory shared across processes.
CORE_API AtomicWaitResult atomic_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout);

// Wakes up to max_count waiters blocked on address; returns the number actually woken.
CORE_API size_t atomic_notify(void* address, size_t size, size_t max_count);

}
