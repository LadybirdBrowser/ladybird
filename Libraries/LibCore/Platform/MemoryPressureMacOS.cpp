/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS)
#    error "This file is only available on macOS"
#endif

#include <LibCore/Platform/MemoryPressure.h>
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>

namespace Core::Platform {

MemoryPressure system_memory_pressure()
{
    // NB: The kernel reports the same levels that a dispatch source of the memory pressure type delivers.
    int level = 0;
    size_t size = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &size, nullptr, 0) < 0)
        return MemoryPressure::Normal;

    switch (level) {
    case DISPATCH_MEMORYPRESSURE_WARN:
        return MemoryPressure::Warning;
    case DISPATCH_MEMORYPRESSURE_CRITICAL:
        return MemoryPressure::Critical;
    default:
        return MemoryPressure::Normal;
    }
}

}
