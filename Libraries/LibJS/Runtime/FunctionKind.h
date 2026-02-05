/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace JS {

enum class FunctionKind : u8 {
    Normal,
    Generator,
    Async,
    AsyncGenerator
};

}
