/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibGC/Export.h>

namespace GC {

// 32 GiB is what a 32-bit cell offset in units of the 8-byte cell alignment can address. It is the same on every target,
// and small enough for the second region-sized reservation used to align its base to fit in the user address space of
// Sv39 RISC-V, 39-bit AArch64, and the application ranges of the sanitizers.
static constexpr size_t HEAP_REGION_SIZE = 32ull * GiB;
static_assert(is_power_of_two(HEAP_REGION_SIZE));
static constexpr size_t HEAP_REGION_OFFSET_MASK = HEAP_REGION_SIZE - 1;

}

extern "C" GC_API FlatPtr js_heap_region_base;
