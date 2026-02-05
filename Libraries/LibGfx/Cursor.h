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
    OpenHand,
    Drag,
    DragCopy,
    Move,
    Wait,
    Disallowed,
    Eyedropper,
    Zoom,
};

struct ImageCursor {
    ShareableBitmap bitmap;
    IntPoint hotspot;

    bool operator==(ImageCursor const& other) const;
};

using Cursor = Variant<StandardCursor, ImageCursor>;

constexpr StringView standard_cursor_to_string(StandardCursor cursor)
{
    switch (cursor) {
    case StandardCursor::None:
        return "None"sv;
    case StandardCursor::Hidden:
        return "Hidden"sv;
    case StandardCursor::Arrow:
        return "Arrow"sv;
    case StandardCursor::Crosshair:
        return "Crosshair"sv;
    case StandardCursor::IBeam:
        return "IBeam"sv;
    case StandardCursor::ResizeHorizontal:
        return "ResizeHorizontal"sv;
    case StandardCursor::ResizeVertical:
        return "ResizeVertical"sv;
    case StandardCursor::ResizeDiagonalTLBR:
        return "ResizeDiagonalTLBR"sv;
    case StandardCursor::ResizeDiagonalBLTR:
        return "ResizeDiagonalBLTR"sv;
    case StandardCursor::ResizeColumn:
        return "ResizeColumn"sv;
    case StandardCursor::ResizeRow:
        return "ResizeRow"sv;
    case StandardCursor::Hand:
        return "Hand"sv;
    case StandardCursor::Help:
        return "Help"sv;
    case StandardCursor::OpenHand:
        return "OpenHand"sv;
    case StandardCursor::Drag:
        return "Drag"sv;
    case StandardCursor::DragCopy:
        return "DragCopy"sv;
    case StandardCursor::Move:
        return "Move"sv;
    case StandardCursor::Wait:
        return "Wait"sv;
    case StandardCursor::Disallowed:
        return "Disallowed"sv;
    case StandardCursor::Eyedropper:
        return "Eyedropper"sv;
    case StandardCursor::Zoom:
        return "Zoom"sv;
    }
    VERIFY_NOT_REACHED();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ImageCursor const&);

template<>
ErrorOr<Gfx::ImageCursor> decode(Decoder&);

}
