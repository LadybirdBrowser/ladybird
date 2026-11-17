/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/SourceLocation.h>
#include <LibGC/Export.h>

namespace GC {

struct GC_API HeapRoot {
    enum class Type {
        CrossHeapMember,
        RegisterPointer,
        Root,
        RootHashMap,
        RootHashTable,
        RootVector,
        StackPointer,
        VM,
    };

    Type type;
    SourceLocation const* location { nullptr };
    Optional<u32> stack_frame_index {};
    // TEMP DIAGNOSTICS
    FlatPtr raw_value { 0 };
    FlatPtr offset_from_stack_top { 0 };
    u8 compressed_half { 0 }; // 0: whole word, 1: low half, 2: high half
};

}
