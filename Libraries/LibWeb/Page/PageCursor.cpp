/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Page/PageCursor.h>

namespace Web {

Gfx::Cursor PageCursor::current() const
{
    MutexLocker locker(m_mutex);
    return m_current;
}

void PageCursor::request(Gfx::Cursor const& cursor)
{
    // FIXME: This check is only approximate. ImageCursors from the same CursorStyleValue share bitmaps, but may
    //        repaint them. So comparing them does not tell you if they are the same image. Also, the image may
    //        change even if the hovered node does not.
    MutexLocker locker(m_mutex);
    if (m_current == cursor)
        return;
    m_current = cursor;
    if (m_send)
        m_send(cursor);
}

void PageCursor::set_send(Send send)
{
    MutexLocker locker(m_mutex);
    m_send = move(send);
}

Gfx::Cursor css_to_gfx_cursor(CSS::CursorPredefined css_cursor)
{
    switch (css_cursor) {
    case CSS::CursorPredefined::Crosshair:
    case CSS::CursorPredefined::Cell:
        return Gfx::StandardCursor::Crosshair;
    case CSS::CursorPredefined::Grab:
        return Gfx::StandardCursor::OpenHand;
    case CSS::CursorPredefined::Grabbing:
        return Gfx::StandardCursor::Drag;
    case CSS::CursorPredefined::Pointer:
        return Gfx::StandardCursor::Hand;
    case CSS::CursorPredefined::Help:
        return Gfx::StandardCursor::Help;
    case CSS::CursorPredefined::None:
        return Gfx::StandardCursor::Hidden;
    case CSS::CursorPredefined::NotAllowed:
        return Gfx::StandardCursor::Disallowed;
    case CSS::CursorPredefined::Text:
    case CSS::CursorPredefined::VerticalText:
        return Gfx::StandardCursor::IBeam;
    case CSS::CursorPredefined::Move:
    case CSS::CursorPredefined::AllScroll:
        return Gfx::StandardCursor::Move;
    case CSS::CursorPredefined::Progress:
    case CSS::CursorPredefined::Wait:
        return Gfx::StandardCursor::Wait;
    case CSS::CursorPredefined::ColResize:
        return Gfx::StandardCursor::ResizeColumn;
    case CSS::CursorPredefined::EResize:
    case CSS::CursorPredefined::WResize:
    case CSS::CursorPredefined::EwResize:
        return Gfx::StandardCursor::ResizeHorizontal;
    case CSS::CursorPredefined::RowResize:
        return Gfx::StandardCursor::ResizeRow;
    case CSS::CursorPredefined::NResize:
    case CSS::CursorPredefined::SResize:
    case CSS::CursorPredefined::NsResize:
        return Gfx::StandardCursor::ResizeVertical;
    case CSS::CursorPredefined::NeResize:
    case CSS::CursorPredefined::SwResize:
    case CSS::CursorPredefined::NeswResize:
        return Gfx::StandardCursor::ResizeDiagonalBLTR;
    case CSS::CursorPredefined::NwResize:
    case CSS::CursorPredefined::SeResize:
    case CSS::CursorPredefined::NwseResize:
        return Gfx::StandardCursor::ResizeDiagonalTLBR;
    case CSS::CursorPredefined::ZoomIn:
    case CSS::CursorPredefined::ZoomOut:
        return Gfx::StandardCursor::Zoom;
    case CSS::CursorPredefined::Default:
        return Gfx::StandardCursor::Arrow;
    case CSS::CursorPredefined::ContextMenu:
    case CSS::CursorPredefined::Alias:
    case CSS::CursorPredefined::Copy:
    case CSS::CursorPredefined::NoDrop:
        // FIXME: No corresponding GFX Standard Cursor, fallthrough to None
    case CSS::CursorPredefined::Auto:
    default:
        return Gfx::StandardCursor::None;
    }
}

}
