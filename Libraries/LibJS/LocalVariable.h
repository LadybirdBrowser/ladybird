/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>

namespace JS {

struct LocalVariable {
    enum class DeclarationKind {
        Var,
        LetOrConst,
        Function,
        ArgumentsObject,
        CatchClauseParameter
    };

    Utf16FlyString name;
    DeclarationKind declaration_kind;
};

}
