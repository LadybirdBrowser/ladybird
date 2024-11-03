/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/SourceLocation.h>

namespace JS {

struct HeapRoot {
    enum class Type {
        HeapFunctionCapturedPointer,
        Handle,
        MarkedVector,
        ConservativeVector,
        RegisterPointer,
        StackPointer,
        VM,
    };

    Type type;
    SourceLocation const* location { nullptr };
};

}
