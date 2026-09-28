/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <LibCore/Export.h>

#if defined(AK_OS_MACOS)
#    include <pthread.h>
#endif

namespace Core::Platform {

// On macOS these are the QoS classes the scheduler uses to choose cores and clock speeds. Other platforms accept and
// ignore them.
enum class ThreadQoS : u8 {
    UserInteractive,
    UserInitiated,
    Default,
    Utility,
    Background,
};

#if defined(AK_OS_MACOS)

// relative_priority is 0 or negative, down to QOS_MIN_RELATIVE_PRIORITY. Threading::Thread::set_priority() goes
// through pthread_setschedparam(), which resets the QoS class, so the two must not be combined on one thread.
CORE_API ErrorOr<void> set_current_thread_qos(ThreadQoS, int relative_priority = 0);

// Applied at pthread_create() time, so it needs no extra syscall and works inside the helper process sandboxes.
CORE_API ErrorOr<void> apply_thread_qos_to_pthread_attributes(pthread_attr_t&, ThreadQoS, int relative_priority = 0);

#else

inline ErrorOr<void> set_current_thread_qos(ThreadQoS, int = 0) { return {}; }

#endif

}
