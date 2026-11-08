/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibGC/Export.h>

// Declares at global scope that a type holds no cell edges, so that a GC::Function may capture it untraced.
#define GC_UNTRACED_CAPTURE(...)                \
    template<>                                  \
    struct GC::IsUntracedCapture<__VA_ARGS__> { \
        static constexpr bool value = true;     \
    }

// Declares at global scope that a type holds no cell edges, other than through callbacks it keeps, which hold the cells
// they use in roots as callbacks outside the GC heap do. Nothing can check what a callback captures, so this is for types
// that can't keep a GC::Function, such as those of libraries below LibGC.
#define GC_UNTRACED_CAPTURE_WITH_ROOTED_CALLBACKS(...)     \
    template<>                                             \
    struct GC::IsUntracedCapture<__VA_ARGS__> {            \
        static constexpr bool value = true;                \
        static constexpr bool callbacks_hold_roots = true; \
    }

// Declares inside a type that it holds no cell edges, for a type that can't be named at global scope.
#define GC_HOLDS_NO_CELL_EDGES using HoldsNoCellEdges = void

namespace GC {

// Whether a type holds no cell edges, so that a GC::Function may capture it untraced. See LibGC/CaptureTraits.h.
template<typename T>
struct IsUntracedCapture;

struct CAPI;
class Cell;
class CellAllocator;
struct CellTypeInfo;
struct CellTypeThunks;
class DeferGC;
class ForeignCell;
class RootImpl;
class Heap;
class HeapGroup;
class CrossHeapMemberBase;
class HeapBlock;
class NanBoxedValue;
class Timer;
class WeakContainer;
class WeakImpl;

template<typename T>
class Function;

template<typename T>
class HeapHashTable;

template<class T>
class Root;

template<class T>
class Ref;

template<class T>
class Ptr;

template<class T>
class Member;

template<class T>
class NonnullMember;

template<class T>
class HeapVector;

template<class T, size_t inline_capacity = 0>
class RootVector;

template<typename T, typename TraitsForT, bool IsOrdered>
class RootHashTable;

template<typename K, typename V>
class WeakHashMap;

}
