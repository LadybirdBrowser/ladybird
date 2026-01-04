/*
 * Copyright (c) 2026, Binary Alley <wc0vp828w5@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS::Bytecode {

enum class FunctionDefinitionKind : u8 {
    FunctionExpression,
    MethodDefinition,
};

}
