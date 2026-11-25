/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Declares inside a type that it holds no cell edges, for a type that can't be named at global scope.
#define GC_HOLDS_NO_CELL_EDGES using HoldsNoCellEdges = void

namespace GC {

// Whether a type holds no cell edges, so that a GC::Function may capture it untraced. See LibGC/CaptureTraits.h.
template<typename T>
struct IsUntracedCapture;

}

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
