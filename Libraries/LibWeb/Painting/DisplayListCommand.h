/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Painting/DisplayListCommandsGenerated.h>

namespace Web::Painting {

#define ENUMERATE_DISPLAY_LIST_COMMANDS(V)                                             \
    V(DrawGlyphRun, draw_glyph_run)                                                    \
    V(FillRect, fill_rect)                                                             \
    V(DrawScaledDecodedImageFrame, draw_scaled_decoded_image_frame)                    \
    V(DrawRepeatedDecodedImageFrame, draw_repeated_decoded_image_frame)                \
    V(DrawRepeatedDisplayList, draw_repeated_display_list)                             \
    V(DrawTiledDecodedImageFrame, draw_tiled_decoded_image_frame)                      \
    V(DrawCompositedContext, draw_composited_context)                                  \
    V(DrawCanvas, draw_canvas)                                                         \
    V(DrawVideoFrame, draw_video_frame)                                                \
    V(Save, save)                                                                      \
    V(SaveLayer, save_layer)                                                           \
    V(Restore, restore)                                                                \
    V(AddClipRect, add_clip_rect)                                                      \
    V(AddClipPath, add_clip_path)                                                      \
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
    V(AddRoundedRectClip, add_rounded_rect_clip)                                       \
    V(PaintNestedDisplayList, paint_nested_display_list)                               \
    V(CompositorScrollNode, compositor_scroll_node)                                    \
    V(CompositorWheelHitTestTarget, compositor_wheel_hit_test_target)                  \
    V(CompositorWheelHitTestTargetWithCornerRadii,                                     \
        compositor_wheel_hit_test_target_with_corner_radii)                            \
    V(CompositorMainThreadWheelEventRegion, compositor_main_thread_wheel_event_region) \
    V(CompositorViewportScrollbar, compositor_viewport_scrollbar)                      \
    V(CompositorBlockingWheelEventRegion, compositor_blocking_wheel_event_region)      \
    V(PaintScrollBar, paint_scrollbar)                                                 \
    V(ApplyEffects, apply_effects)

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

constexpr int display_list_command_nesting_level_change(DisplayListCommandType command_type)
{
    switch (command_type) {
    case DisplayListCommandType::Save:
    case DisplayListCommandType::SaveLayer:
    case DisplayListCommandType::ApplyEffects:
        return 1;
    case DisplayListCommandType::Restore:
        return -1;
    default:
        return 0;
    }
}

inline Gfx::IntRect DrawGlyphRun::bounding_rect() const { return glyph_bounding_rect; }
inline Gfx::IntRect FillRect::bounding_rect() const { return rect; }
inline Gfx::IntRect DrawScaledDecodedImageFrame::bounding_rect() const { return Gfx::enclosing_int_rect(dst_rect); }
inline Gfx::IntRect DrawRepeatedDecodedImageFrame::bounding_rect() const { return clip_rect; }
inline Gfx::IntRect DrawRepeatedDisplayList::bounding_rect() const { return clip_rect; }
inline Gfx::IntRect DrawTiledDecodedImageFrame::bounding_rect() const { return clip_rect; }
inline Gfx::IntRect DrawCompositedContext::bounding_rect() const { return dst_rect; }
inline Gfx::IntRect DrawCanvas::bounding_rect() const { return dst_rect; }
inline Gfx::IntRect DrawVideoFrame::bounding_rect() const { return dst_rect; }
inline Gfx::IntRect AddClipRect::bounding_rect() const { return Gfx::enclosing_int_rect(rect); }
inline bool AddClipRect::is_clip() const { return true; }
inline Gfx::IntRect AddClipPath::bounding_rect() const { return path_bounding_rect; }
inline bool AddClipPath::is_clip() const { return true; }
inline Gfx::IntRect PaintLinearGradient::bounding_rect() const { return gradient_rect; }
inline Gfx::IntRect PaintTextShadow::bounding_rect() const { return { draw_location.to_type<int>(), shadow_bounding_rect.size() }; }
inline Gfx::IntRect FillRectWithRoundedCorners::bounding_rect() const { return rect; }
inline Gfx::IntRect FillPath::bounding_rect() const { return Gfx::enclosing_int_rect(path_bounding_rect); }
inline Gfx::IntRect StrokePath::bounding_rect() const { return Gfx::enclosing_int_rect(path_bounding_rect); }
inline Gfx::IntRect DrawEllipse::bounding_rect() const { return rect; }
inline Gfx::IntRect DrawLine::bounding_rect() const { return Gfx::IntRect::from_two_points(from, to).inflated(thickness, thickness); }
inline Gfx::IntRect ApplyBackdropFilter::bounding_rect() const { return backdrop_region; }
inline Gfx::IntRect DrawRect::bounding_rect() const { return rect; }
inline Gfx::IntRect PaintRadialGradient::bounding_rect() const { return rect; }
inline Gfx::IntRect PaintConicGradient::bounding_rect() const { return rect; }
inline Gfx::IntRect AddRoundedRectClip::bounding_rect() const { return border_rect; }
inline bool AddRoundedRectClip::is_clip() const { return true; }
inline Gfx::IntRect PaintNestedDisplayList::bounding_rect() const { return Gfx::enclosing_int_rect(rect); }
inline Gfx::IntRect PaintScrollBar::bounding_rect() const { return track_rect.united(thumb_rect); }

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
static_assert(sizeof(DisplayListCommandRun) == 44);
static_assert(IsTriviallyCopyable<DisplayListGlyph>);

inline bool operator==(DisplayListCommandRun const& a, DisplayListCommandRun const& b)
{
    return a.offset == b.offset
        && a.size == b.size
        && a.context == b.context
        && a.ink_bounds == b.ink_bounds
        && a.nesting_delta == b.nesting_delta
        && a.min_relative_nesting == b.min_relative_nesting
        && a.has_unbounded_draw == b.has_unbounded_draw
        && a.has_compositor_metadata == b.has_compositor_metadata
        && a.has_unconfined_clip == b.has_unconfined_clip
        && a.is_self_contained == b.is_self_contained;
}

#define VERIFY_DISPLAY_LIST_COMMAND(command, player_method) static_assert(IsTriviallyCopyable<command>);
ENUMERATE_DISPLAY_LIST_COMMANDS(VERIFY_DISPLAY_LIST_COMMAND)
#undef VERIFY_DISPLAY_LIST_COMMAND

}
