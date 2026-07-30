/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>

namespace JS {

using BreakpointID = u32;

struct Breakpoint {
    BreakpointID id { 0 };
    Utf16String filename;
    u32 line { 0 };
    Optional<u32> column;
};

}
