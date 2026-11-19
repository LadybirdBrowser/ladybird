/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibCore/Export.h>

namespace Core::Platform {

// How short of memory the system as a whole is, as far as the operating system lets us tell.
enum class MemoryPressure : u8 {
    Normal,
    // The system is short of memory, and has started to compress or swap it out.
    Warning,
    // The system is about to run out of memory, and will soon end processes to get some back.
    Critical,
};

// NB: This is cheap enough to call every few seconds. Where the system can't tell, the pressure is always normal.
CORE_API MemoryPressure system_memory_pressure();

}
