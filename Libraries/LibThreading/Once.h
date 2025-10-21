/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Concepts.h>
#include <LibThreading/Mutex.h>

namespace Threading {

struct OnceFlag {
    Mutex mutex;
    Atomic<bool> has_been_called { false };
};

template<VoidFunction Callable>
void call_once(OnceFlag& flag, Callable&& callable)
{
    if (!flag.has_been_called.load(MemoryOrder::memory_order_acquire)) {
        MutexLocker lock(flag.mutex);

        // Another thread may have called the function while we were waiting on the mutex
        // The mutex guarantees exclusivity so we can use relaxed ordering
        if (flag.has_been_called.load(MemoryOrder::memory_order_relaxed))
            return;

        callable();
        flag.has_been_called.store(true, MemoryOrder::memory_order_release);
    }
}

}
