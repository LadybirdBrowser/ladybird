/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

}
