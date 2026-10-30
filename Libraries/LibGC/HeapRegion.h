/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
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

// The bottom of the region is reserved but never committed, so no cell has an offset below 4 GiB. A compressed cell
// pointer below 512 Mi then never decodes to a cell. That keeps small integers from looking like one, and also the low
// half of a pointer to memory less than 512 MiB into any mapping aligned to 4 GiB or more, such as this region itself.
// Such low halves are all over the stack, so a smaller guard has the conservative scanner keep cells alive for them.
static constexpr size_t HEAP_REGION_LOW_GUARD_SIZE = 4ull * GiB;

// The top of the region is reserved but never committed either. Decoding any offset lands inside the region, and this
// keeps a field access through the decoded address from reaching memory past the end of the reservation.
static constexpr size_t HEAP_REGION_HIGH_GUARD_SIZE = 2ull * MiB;

}

extern "C" GC_API FlatPtr js_heap_region_base;

namespace GC {

// A copy of the heap region base outside LibGC, which LibGC fills in when it reserves the region, or when the copy is
// registered if that happens later. Either way that is before any cell exists, and the base never changes after that.
// A registration lasts for the rest of the process, like the shared objects that hold them, which are never unloaded.
class GC_API HeapRegionBaseCopyRegistration {
    AK_MAKE_NONCOPYABLE(HeapRegionBaseCopyRegistration);
    AK_MAKE_NONMOVABLE(HeapRegionBaseCopyRegistration);

public:
    explicit HeapRegionBaseCopyRegistration(FlatPtr& copy);

private:
    friend class HeapRegion;

    FlatPtr* m_copy { nullptr };
    HeapRegionBaseCopyRegistration* m_next { nullptr };
};

namespace Detail {

// OPTIMIZATION: Every shared object keeps a hidden copy of the base, which a decode reads with one PC-relative load.
//               Reading js_heap_region_base from outside LibGC takes a GOT load first.
class HeapRegionBaseCopy {
public:
    FlatPtr base() const { return m_base; }

private:
    // NB: Static storage starts out zeroed, so the base is 0 until registering it fills it in, also for anything that
    //     reads it before this is constructed.
    FlatPtr m_base { 0 };
    HeapRegionBaseCopyRegistration m_registration { m_base };
};

// NB: Every translation unit that decodes includes this header, so the copy of its shared object is constructed and
//     registered before the dynamic initialization of anything that it defines after including it. No cell exists
//     before the region does, so the copy holds the base by the time anything decodes a cell pointer.
#if defined(AK_OS_WINDOWS)
inline HeapRegionBaseCopy heap_region_base_copy;
#else
[[gnu::visibility("hidden")]] inline HeapRegionBaseCopy heap_region_base_copy;
#endif

}

ALWAYS_INLINE FlatPtr heap_region_base()
{
    return Detail::heap_region_base_copy.base();
}

}
