/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibWeb/PixelUnits.h>
#include <UI/Qt/InputMethodUtils.h>

#include <QRectF>
#include <QString>

// The platform input method decides whether to compose from inputMethodQuery(Qt::ImEnabled). WebContent pushes
// InputMethodState to the UI (did_update_input_method_state), and the Qt view answers inputMethodQuery from the
// most-recent push via input_method_query_for_state(). If that stops tracking is_enabled, the platform input method
// silently never starts composing — and input from dead keys and IMEs into editable content is dropped (#11182).

TEST_CASE(enabled_state_maps_to_im_enabled_and_im_read_only)
{
    WebView::ViewImplementation::InputMethodState state;
    state.is_enabled = true;

    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImEnabled, 1.0).toBool());
    EXPECT(!Ladybird::input_method_query_for_state(state, Qt::ImReadOnly, 1.0).toBool());

    state.is_enabled = false;
    EXPECT(!Ladybird::input_method_query_for_state(state, Qt::ImEnabled, 1.0).toBool());
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImReadOnly, 1.0).toBool());

    // Both queries must be answered from the state (a false bool is still a valid answer), never left to the base widget.
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImEnabled, 1.0).isValid());
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImReadOnly, 1.0).isValid());
}

TEST_CASE(cursor_and_surrounding_text_map_to_queries)
{
    WebView::ViewImplementation::InputMethodState state {
        .is_enabled = true,
        .cursor_position = 3,
        .anchor_position = 1,
        .text_before_cursor = "ab"_utf16,
        .text_after_cursor = "c"_utf16,
        .caret_rect = {},
    };

    EXPECT_EQ(Ladybird::input_method_query_for_state(state, Qt::ImCursorPosition, 1.0).toInt(), 3);
    EXPECT_EQ(Ladybird::input_method_query_for_state(state, Qt::ImAbsolutePosition, 1.0).toInt(), 3);
    EXPECT_EQ(Ladybird::input_method_query_for_state(state, Qt::ImAnchorPosition, 1.0).toInt(), 1);
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImTextBeforeCursor, 1.0).toString() == QStringLiteral("ab"));
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImTextAfterCursor, 1.0).toString() == QStringLiteral("c"));
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImSurroundingText, 1.0).toString() == QStringLiteral("abc"));
}

TEST_CASE(caret_rect_scales_by_device_pixel_ratio_and_falls_back_when_absent)
{
    WebView::ViewImplementation::InputMethodState state;
    state.caret_rect = Web::DevicePixelRect { 10, 20, 2, 30 };

    // Device pixels map to logical pixels; width and height are clamped to at least 1 — so the input-method overlay
    // anchor is never empty.
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImCursorRectangle, 2.0).toRectF() == QRectF(5, 10, 1, 15));
    EXPECT(Ladybird::input_method_query_for_state(state, Qt::ImAnchorRectangle, 2.0).toRectF() == QRectF(5, 10, 1, 15));

    // Without a caret rect (and for queries the state cannot answer) an invalid QVariant tells the caller to fall
    // back to the base widget's answer.
    state.caret_rect = {};
    EXPECT(!Ladybird::input_method_query_for_state(state, Qt::ImCursorRectangle, 2.0).isValid());
    EXPECT(!Ladybird::input_method_query_for_state(state, Qt::ImHints, 1.0).isValid());
}
