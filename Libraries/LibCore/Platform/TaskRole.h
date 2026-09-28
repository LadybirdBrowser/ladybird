/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Platform.h>
#include <LibCore/Export.h>

namespace Core::Platform {

#if defined(AK_OS_MACOS)

// A process spawned by an application counts as an application without a role, and the kernel squashes the
// user-interactive and user-initiated QoS classes of such a task down to the default class. The foreground application
// role lifts that ceiling and puts the task's latency and throughput tiers where a visible application's are. Chromium's
// child processes do the same at startup; WebKit's get it from their XPC service type.
CORE_API ErrorOr<void> adopt_foreground_application_task_role();

#else

inline ErrorOr<void> adopt_foreground_application_task_role() { return {}; }

#endif

}
