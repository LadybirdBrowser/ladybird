/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace JS {

struct LocalVariable {
    FlyString name;
    enum class DeclarationKind {
        Var,
        LetOrConst,
        Function,
        ArgumentsObject,
        CatchClauseParameter
    };
    DeclarationKind declaration_kind;
};

}
