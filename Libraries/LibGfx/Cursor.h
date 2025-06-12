/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibGfx/Point.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Forward.h>

namespace Gfx {

enum class StandardCursor {
    None = 0,
    Hidden,
    Arrow,
    Crosshair,
    IBeam,
    ResizeHorizontal,
    ResizeVertical,
    ResizeDiagonalTLBR,
    ResizeDiagonalBLTR,
    ResizeColumn,
    ResizeRow,
    Hand,
    Help,
    Drag,
    DragCopy,
    Move,
    Wait,
    Disallowed,
    Eyedropper,
    Zoom,
    __Count,
};

struct ImageCursor {
    ShareableBitmap bitmap;
    IntPoint hotspot;

    bool operator==(ImageCursor const& other) const;
};

using Cursor = Variant<StandardCursor, ImageCursor>;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ImageCursor const&);

template<>
ErrorOr<Gfx::ImageCursor> decode(Decoder&);

}
