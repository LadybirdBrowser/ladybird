/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/DecodedImageFrame.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Painting {

struct DragImageOverlay {
    Gfx::DecodedImageFrame frame;
    CSSPixelRect rect;

    bool operator==(DragImageOverlay const& other) const { return frame.id() == other.frame.id() && rect == other.rect; }
};

}
