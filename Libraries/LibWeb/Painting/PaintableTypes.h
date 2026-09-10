/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

enum class PaintCommandCacheMode : u8 {
    ReadOnly,
    ReadWrite,
};

enum class PaintPhase {
    Background,
    Border,
    TableCollapsedBorder,
    Foreground,
    Outline,
    Overlay,
};

enum class SelectionState : u8 {
    None,
    Start,
    End,
    StartAndEnd,
    Full,
};

}
