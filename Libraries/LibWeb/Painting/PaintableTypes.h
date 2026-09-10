/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::Painting {

enum class PaintCommandCacheMode : u8 {
    ReadOnly,
    ReadWrite,
};

enum class SelectionState : u8 {
    None,
    Start,
    End,
    StartAndEnd,
    Full,
};

}
