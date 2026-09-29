/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <UI/Qt/InputMethodUtils.h>
#include <UI/Qt/StringUtils.h>

#include <QRectF>

namespace Ladybird {

static Optional<QRectF> input_method_rect_for_caret(Optional<Web::DevicePixelRect> const& caret_rect, double device_pixel_ratio)
{
    if (!caret_rect.has_value())
        return {};

    return QRectF {
        caret_rect->x().value() / device_pixel_ratio,
        caret_rect->y().value() / device_pixel_ratio,
        max(caret_rect->width().value() / device_pixel_ratio, 1.0),
        max(caret_rect->height().value() / device_pixel_ratio, 1.0),
    };
}

QVariant input_method_query_for_state(WebView::ViewImplementation::InputMethodState const& state, Qt::InputMethodQuery query, double device_pixel_ratio)
{
    switch (query) {
    case Qt::ImEnabled:
        return state.is_enabled;
    case Qt::ImCursorRectangle:
    case Qt::ImAnchorRectangle:
        if (auto rect = input_method_rect_for_caret(state.caret_rect, device_pixel_ratio); rect.has_value())
            return *rect;
        return {};
    case Qt::ImAbsolutePosition:
    case Qt::ImCursorPosition:
        return state.cursor_position;
    case Qt::ImAnchorPosition:
        return state.anchor_position;
    case Qt::ImTextBeforeCursor:
        return qstring_from_utf16_string(state.text_before_cursor);
    case Qt::ImTextAfterCursor:
        return qstring_from_utf16_string(state.text_after_cursor);
    case Qt::ImSurroundingText:
        return qstring_from_utf16_string(state.text_before_cursor) + qstring_from_utf16_string(state.text_after_cursor);
    case Qt::ImReadOnly:
        return !state.is_enabled;
    default:
        return {};
    }
}

}
