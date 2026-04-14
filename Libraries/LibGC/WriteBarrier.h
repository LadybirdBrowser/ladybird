/*
 * Copyright (c) 2026, Marc Butler <marc@mailworks.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Forward.h>

namespace GC {

// Hybrid SATB + Dijkstra write barrier for incremental marking.
//
// SATB (deletion barrier): shades the OLD target gray to preserve the
// snapshot-at-the-beginning invariant.
//
// Dijkstra (insertion barrier): shades the NEW target gray so that edges
// created after a cell is traced are still discovered.
//
// The fast path (checking s_incremental_marking_active) is inlined.
// The slow path (shade_gray_slow) is out-of-line in Heap.cpp.

GC_API extern bool s_incremental_marking_active;

GC_API void shade_gray_slow(Cell* old_target);

template<typename T>
ALWAYS_INLINE void write_barrier(T* old_target, T* new_target)
{
    if (!s_incremental_marking_active) [[likely]]
        return;
    if (old_target)
        shade_gray_slow(reinterpret_cast<Cell*>(const_cast<RemoveConst<T>*>(old_target)));
    if (new_target)
        shade_gray_slow(reinterpret_cast<Cell*>(const_cast<RemoveConst<T>*>(new_target)));
}

// Write barrier for NanBoxedValue stores.
// Extracts the cell pointer (if any) from old and new values and shades gray.
GC_API void value_write_barrier(NanBoxedValue const& old_value);
GC_API void value_write_barrier(NanBoxedValue const& old_value, NanBoxedValue const& new_value);

}
