/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Painting/DisplayListCommandsGenerated.h>

namespace Web::Painting {

#define ENUMERATE_DISPLAY_LIST_COMMANDS(V)                                             \
    V(DrawGlyphRun, draw_glyph_run)                                                    \
    V(FillRect, fill_rect)                                                             \
    V(PaintCaret, paint_caret)                                                         \
    V(DrawScaledDecodedImageFrame, draw_scaled_decoded_image_frame)                    \
    V(DrawRepeatedDecodedImageFrame, draw_repeated_decoded_image_frame)                \
    V(DrawRepeatedDisplayList, draw_repeated_display_list)                             \
    V(DrawTiledDecodedImageFrame, draw_tiled_decoded_image_frame)                      \
    V(DrawCompositedContext, draw_composited_context)                                  \
    V(DrawCanvas, draw_canvas)                                                         \
    V(DrawVideoFrame, draw_video_frame)                                                \
    V(PaintLinearGradient, paint_linear_gradient)                                      \
    V(PaintRadialGradient, paint_radial_gradient)                                      \
    V(PaintConicGradient, paint_conic_gradient)                                        \
    V(PaintOuterBoxShadow, paint_outer_box_shadow)                                     \
    V(PaintInnerBoxShadow, paint_inner_box_shadow)                                     \
    V(PaintTextShadow, paint_text_shadow)                                              \
    V(FillRectWithRoundedCorners, fill_rect_with_rounded_corners)                      \
    V(FillPath, fill_path)                                                             \
    V(StrokePath, stroke_path)                                                         \
    V(DrawEllipse, draw_ellipse)                                                       \
    V(DrawLine, draw_line)                                                             \
    V(ApplyBackdropFilter, apply_backdrop_filter)                                      \
    V(DrawRect, draw_rect)                                                             \
    V(PaintNestedDisplayList, paint_nested_display_list)                               \
    V(DrawIsolatedDisplayList, draw_isolated_display_list)                             \
    V(CompositorScrollNode, compositor_scroll_node)                                    \
    V(CompositorWheelHitTestTarget, compositor_wheel_hit_test_target)                  \
    V(CompositorWheelHitTestTargetWithCornerRadii,                                     \
        compositor_wheel_hit_test_target_with_corner_radii)                            \
    V(CompositorMainThreadWheelEventRegion, compositor_main_thread_wheel_event_region) \
    V(CompositorViewportScrollbar, compositor_viewport_scrollbar)                      \
    V(CompositorBlockingWheelEventRegion, compositor_blocking_wheel_event_region)      \
    V(PaintScrollBar, paint_scrollbar)

constexpr bool display_list_command_is_compositor_metadata(DisplayListCommandType type)
{
    switch (type) {
    case DisplayListCommandType::CompositorScrollNode:
    case DisplayListCommandType::CompositorWheelHitTestTarget:
    case DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii:
    case DisplayListCommandType::CompositorMainThreadWheelEventRegion:
    case DisplayListCommandType::CompositorViewportScrollbar:
    case DisplayListCommandType::CompositorBlockingWheelEventRegion:
        return true;
    default:
        return false;
    }
}

constexpr i64 caret_blink_interval_ns = 500'000'000;
inline bool caret_is_visible_at_time(PaintCaret const& caret, i64 monotonic_time_ns)
{
    if (!caret.should_blink)
        return true;
    auto elapsed_ns = monotonic_time_ns > caret.blink_cycle_start_time_ns
        ? monotonic_time_ns - caret.blink_cycle_start_time_ns
        : 0;
    return (elapsed_ns / caret_blink_interval_ns) % 2 == 0;
}

template<typename Command>
concept DisplayListCommand = requires {
    Command::command_type;
};

template<typename T>
requires(IsTriviallyCopyable<T>)
ReadonlyBytes display_list_object_bytes(T const& object)
{
    return { &object, sizeof(T) };
}

template<typename T>
requires(IsTriviallyCopyable<T>)
T read_display_list_object(ReadonlyBytes bytes)
{
    VERIFY(bytes.size() >= sizeof(T));
    T object;
    __builtin_memcpy(&object, bytes.data(), sizeof(T));
    return object;
}

template<typename T>
requires(IsTriviallyCopyable<T>)
void write_display_list_object(Bytes bytes, T const& object)
{
    VERIFY(bytes.size() >= sizeof(T));
    __builtin_memcpy(bytes.data(), &object, sizeof(T));
}

template<DisplayListCommand Command>
Command read_display_list_command_payload(ReadonlyBytes payload)
{
    return read_display_list_object<Command>(payload);
}

template<typename Callback>
decltype(auto) visit_display_list_command_type(DisplayListCommandType command_type, Callback&& callback)
{
    switch (command_type) {
#define VISIT_DISPLAY_LIST_COMMAND_TYPE(command, player_method) \
    case DisplayListCommandType::command:                       \
        return callback.template operator()<command>();
        ENUMERATE_DISPLAY_LIST_COMMANDS(VISIT_DISPLAY_LIST_COMMAND_TYPE)
#undef VISIT_DISPLAY_LIST_COMMAND_TYPE
    }
    VERIFY_NOT_REACHED();
}

template<typename Callback>
decltype(auto) visit_display_list_command(
    DisplayListCommandType command_type,
    ReadonlyBytes payload,
    Callback&& callback)
{
    return visit_display_list_command_type(command_type, [&]<DisplayListCommand Command>() -> decltype(auto) {
        return callback(read_display_list_command_payload<Command>(payload));
    });
}

static_assert(IsTriviallyCopyable<DisplayListCommandHeader>);
static_assert(sizeof(DisplayListCommandHeader) == 32);
static_assert(IsTriviallyCopyable<DisplayListCommandRun>);
static_assert(sizeof(DisplayListCommandRun) == 36);
static_assert(IsTriviallyCopyable<DisplayListGlyph>);
static_assert(IsTriviallyCopyable<DisplayListInlineClip>);
static_assert(sizeof(DisplayListInlineClip) == 64);

template<typename Callback>
void for_each_display_list_inline_clip(DisplayListCommandHeader const& header, ReadonlyBytes payload, Callback&& callback)
{
    size_t entries_size = header.inline_clip_count * sizeof(DisplayListInlineClip);
    VERIFY(entries_size <= payload.size());
    size_t entry_offset = payload.size() - entries_size;
    for (u8 index = 0; index < header.inline_clip_count; ++index, entry_offset += sizeof(DisplayListInlineClip))
        callback(read_display_list_object<DisplayListInlineClip>(payload.slice(entry_offset)));
}

inline bool operator==(DisplayListCommandRun const& a, DisplayListCommandRun const& b)
{
    return a.offset == b.offset
        && a.size == b.size
        && a.context == b.context
        && a.ink_bounds == b.ink_bounds
        && a.has_unbounded_draw == b.has_unbounded_draw
        && a.has_compositor_metadata == b.has_compositor_metadata;
}

#define VERIFY_DISPLAY_LIST_COMMAND(command, player_method) static_assert(IsTriviallyCopyable<command>);
ENUMERATE_DISPLAY_LIST_COMMANDS(VERIFY_DISPLAY_LIST_COMMAND)
#undef VERIFY_DISPLAY_LIST_COMMAND

}
