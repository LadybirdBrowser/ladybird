/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/SourceLocation.h>

namespace GC {

struct GC_API HeapRoot {
    enum class Type {
        HeapFunctionCapturedPointer,
        Root,
        RootVector,
        RootHashMap,
        ConservativeVector,
        RegisterPointer,
        StackPointer,
        VM,
    };

    Type type;
    SourceLocation const* location { nullptr };
};

}
