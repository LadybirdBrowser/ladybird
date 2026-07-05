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
//
// PLATFORM CAVEAT: where no cross-process wait primitive is wired up, this polls for a change to the word — instead of
// waiting for a notification. See poll_wait() in Futex.cpp. On that path, a wake is inferred from the store — not from
// the notification — which in practice the waker protocols (Emscripten's included) always perform first. What it can't
// observe is a notify that leaves the word unchanged: Such a notify wakes nobody — so an untimed wait paired only with
// one blocks until its agent goes away. That path is only taken on platforms other than Linux and modern macOS.
CORE_API AtomicWaitResult atomic_wait(void* address, u64 expected, size_t size, Optional<AK::Duration> timeout);

// Wakes up to max_count waiters blocked on address; returns the number actually woken. On the polling path described
// above, there's nothing to signal, and no waiter registry to count — so, this reports zero however many waiters exist
// (a count Atomics.notify hands straight back to script).
CORE_API size_t atomic_notify(void* address, size_t size, size_t max_count);

}
