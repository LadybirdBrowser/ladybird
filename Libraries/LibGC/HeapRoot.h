/*
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/SourceLocation.h>

namespace GC {

struct GC_API HeapRoot {
    enum class Type {
        ConservativeVector,
        HeapFunctionCapturedPointer,
        MustSurviveGC,
        RegisterPointer,
        Root,
        RootHashMap,
        RootVector,
        StackPointer,
        VM,
    };

    Type type;
    SourceLocation const* location { nullptr };
};

}
