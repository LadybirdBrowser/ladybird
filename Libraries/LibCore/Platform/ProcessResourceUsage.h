/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibCore/Platform/ProcessStatistics.h>

namespace Core::Platform {

struct ProcessResourceUsage {
    u64 cpu_time_microseconds { 0 };
    u64 attributed_memory_bytes { 0 };
};

CORE_API Optional<ProcessResourceUsage> process_resource_usage(ProcessInfo const&);

}
