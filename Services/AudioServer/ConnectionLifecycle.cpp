/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AudioServer/ConnectionLifecycle.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/BackgroundAction.h>

namespace AudioServer {

static Atomic<size_t> s_connection_count { 0 };

void register_connection()
{
    s_connection_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

void unregister_connection()
{
    size_t old_value = s_connection_count.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel);
    if (old_value == 1) {
        Threading::quit_background_thread();
        Core::EventLoop::current().quit(0);
    }
}

}
