/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Platform/MemoryPressure.h>

namespace Core::Platform {

MemoryPressure system_memory_pressure()
{
    return MemoryPressure::Normal;
}

}
