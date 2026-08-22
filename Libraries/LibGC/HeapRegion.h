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

#if AK_IS_ARCH_RISCV64()
// Sv39 provides 256 GiB of user virtual address space after splitting the
// address space between userspace and the kernel. Keep room for the second
// region-sized reservation used to align the cage base.
static constexpr size_t HEAP_REGION_SIZE = 64ull * GiB;
#elif AK_IS_ARCH_AARCH64() && !defined(AK_OS_MACOS)
// Some AArch64 systems provide only 39 or 42 bits of user virtual address
// space. Keep the cage small enough to fit those systems, including the
// second region-sized reservation used to align its base.
static constexpr size_t HEAP_REGION_SIZE = 128ull * GiB;
#else
static constexpr size_t HEAP_REGION_SIZE = 4ull * TiB;
#endif
static_assert(is_power_of_two(HEAP_REGION_SIZE));
static constexpr size_t HEAP_REGION_OFFSET_MASK = HEAP_REGION_SIZE - 1;

}

extern "C" GC_API FlatPtr js_heap_region_base;
