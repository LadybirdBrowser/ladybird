/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ShouldAntiAlias.h>

namespace Web::Painting {

class DisplayList;

#define ENUMERATE_DISPLAY_LIST_COMMANDS(V)                              \
    V(DrawGlyphRun, draw_glyph_run)                                     \
    V(FillRect, fill_rect)                                              \
    V(DrawScaledDecodedImageFrame, draw_scaled_decoded_image_frame)     \
    V(DrawRepeatedDecodedImageFrame, draw_repeated_decoded_image_frame) \
    V(DrawExternalContent, draw_external_content)                       \
    V(DrawVideoFrameSource, draw_video_frame_source)                    \
    V(Save, save)                                                       \
    V(SaveLayer, save_layer)                                            \
    V(Restore, restore)                                                 \
    V(Translate, translate)                                             \
    V(AddClipRect, add_clip_rect)                                       \
    V(PaintLinearGradient, paint_linear_gradient)                       \
    V(PaintRadialGradient, paint_radial_gradient)                       \
    V(PaintConicGradient, paint_conic_gradient)                         \
    V(PaintOuterBoxShadow, paint_outer_box_shadow)                      \
    V(PaintInnerBoxShadow, paint_inner_box_shadow)                      \
    V(PaintTextShadow, paint_text_shadow)                               \
    V(FillRectWithRoundedCorners, fill_rect_with_rounded_corners)       \
    V(FillPath, fill_path)                                              \
    V(StrokePath, stroke_path)                                          \
    V(DrawEllipse, draw_ellipse)                                        \
    V(FillEllipse, fill_ellipse)                                        \
    V(DrawLine, draw_line)                                              \
    V(ApplyBackdropFilter, apply_backdrop_filter)                       \
    V(DrawRect, draw_rect)                                              \
    V(AddRoundedRectClip, add_rounded_rect_clip)                        \
    V(PaintNestedDisplayList, paint_nested_display_list)                \
    V(PaintScrollBar, paint_scrollbar)                                  \
    V(ApplyEffects, apply_effects)

enum class DisplayListCommandType : u8 {
#define ENUMERATE_DISPLAY_LIST_COMMAND_TYPE(command, player_method) command,
    ENUMERATE_DISPLAY_LIST_COMMANDS(ENUMERATE_DISPLAY_LIST_COMMAND_TYPE)
#undef ENUMERATE_DISPLAY_LIST_COMMAND_TYPE
};

struct DisplayListDataSpan {
    u32 offset { 0 };
    u32 size { 0 };

    [[nodiscard]] bool is_empty() const { return size == 0; }
};

struct DisplayListColorInterpolationMethod {
    enum class Type : u8 {
        Rectangular,
        Polar,
    };

    Type type { Type::Rectangular };
    CSS::RectangularColorSpace rectangular_color_space { CSS::RectangularColorSpace::Oklab };
    CSS::PolarColorSpace polar_color_space { CSS::PolarColorSpace::Oklch };
    CSS::HueInterpolationMethod hue_interpolation_method { CSS::HueInterpolationMethod::Shorter };
};

struct DisplayListGradientColorStops {
    DisplayListDataSpan colors;
    DisplayListDataSpan positions;
    bool repeating { false };
};

struct DisplayListCommandHeader {
    DisplayListCommandType type;
    u32 payload_size { 0 };
    VisualContextIndex context_index {};
    bool has_bounding_rect { false };
    bool is_clip { false };
    Gfx::IntRect bounding_rect {};
};

struct DrawGlyphRun {
    static constexpr StringView command_name = "DrawGlyphRun"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawGlyphRun;

    FontResourceId font_id;
    DisplayListDataSpan glyphs;
    Gfx::IntRect rect;
    Gfx::IntRect glyph_bounding_rect;
    Gfx::FloatPoint translation;
    float scale { 1.0f };
    Color color;
    Gfx::Orientation orientation { Gfx::Orientation::Horizontal };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return glyph_bounding_rect; }
    void dump(StringBuilder&) const;
};

struct FillRect {
    static constexpr StringView command_name = "FillRect"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::FillRect;

    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

struct DrawScaledDecodedImageFrame {
    static constexpr StringView command_name = "DrawScaledDecodedImageFrame"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawScaledDecodedImageFrame;

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    ImageFrameResourceId frame_id;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
    void dump(StringBuilder&) const;
};

struct DrawRepeatedDecodedImageFrame {
    static constexpr StringView command_name = "DrawRepeatedDecodedImageFrame"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawRepeatedDecodedImageFrame;

    struct Repeat {
        bool x { false };
        bool y { false };
    };

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    ImageFrameResourceId frame_id;
    Gfx::ScalingMode scaling_mode;
    Repeat repeat;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
    void dump(StringBuilder&) const;
};

struct DrawExternalContent {
    static constexpr StringView command_name = "DrawExternalContent"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawExternalContent;

    Gfx::IntRect dst_rect;
    ExternalContentResourceId source_id;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void dump(StringBuilder&) const;
};

struct DrawVideoFrameSource {
    static constexpr StringView command_name = "DrawVideoFrameSource"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawVideoFrameSource;

    Gfx::IntRect dst_rect;
    VideoFrameResourceId source_id;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void dump(StringBuilder&) const;
};

struct Save {
    static constexpr StringView command_name = "Save"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::Save;
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct SaveLayer {
    static constexpr StringView command_name = "SaveLayer"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::SaveLayer;
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct Restore {
    static constexpr StringView command_name = "Restore"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::Restore;
    static constexpr int nesting_level_change = -1;

    void dump(StringBuilder&) const;
};

struct Translate {
    static constexpr StringView command_name = "Translate"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::Translate;

    Gfx::IntPoint delta;

    void dump(StringBuilder&) const;
};

struct AddClipRect {
    static constexpr StringView command_name = "AddClipRect"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::AddClipRect;

    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip() const { return true; }
    void dump(StringBuilder&) const;
};

struct PaintLinearGradient {
    static constexpr StringView command_name = "PaintLinearGradient"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintLinearGradient;

    Gfx::IntRect gradient_rect;
    float gradient_angle { 0.0f };
    DisplayListGradientColorStops color_stops;
    float first_stop_position { 0.0f };
    float repeat_length { 1.0f };
    DisplayListColorInterpolationMethod interpolation_method;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return gradient_rect; }

    void dump(StringBuilder&) const;
};

struct PaintOuterBoxShadow {
    static constexpr StringView command_name = "PaintOuterBoxShadow"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintOuterBoxShadow;

    Gfx::Color color;
    int blur_radius;
    Gfx::IntRect device_content_rect;
    CornerRadii content_corner_radii;
    Gfx::IntRect shadow_rect;
    CornerRadii shadow_corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintInnerBoxShadow {
    static constexpr StringView command_name = "PaintInnerBoxShadow"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintInnerBoxShadow;

    Gfx::Color color;
    int blur_radius;
    Gfx::IntRect device_content_rect;
    CornerRadii content_corner_radii;
    Gfx::IntRect outer_shadow_rect;
    Gfx::IntRect inner_shadow_rect;
    CornerRadii inner_shadow_corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintTextShadow {
    static constexpr StringView command_name = "PaintTextShadow"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintTextShadow;

    FontResourceId font_id;
    DisplayListDataSpan glyphs;
    Gfx::IntRect shadow_bounding_rect;
    Gfx::IntRect text_rect;
    Gfx::FloatPoint draw_location;
    float scale { 1.0f };
    int blur_radius;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return { draw_location.to_type<int>(), shadow_bounding_rect.size() }; }
    void dump(StringBuilder&) const;
};

struct FillRectWithRoundedCorners {
    static constexpr StringView command_name = "FillRectWithRoundedCorners"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::FillRectWithRoundedCorners;

    Gfx::IntRect rect;
    Color color;
    CornerRadii corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

enum class PathPaintKind : u8 {
    Color,
    PaintStyle,
};

struct FillPath {
    static constexpr StringView command_name = "FillPath"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::FillPath;

    Gfx::IntRect path_bounding_rect;
    DisplayListDataSpan path_data;
    float opacity { 1.0f };
    PathPaintKind paint_kind { PathPaintKind::Color };
    Color color;
    PaintStyleResourceId paint_style_id;
    Gfx::WindingRule winding_rule;
    ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void dump(StringBuilder&) const;
};

struct StrokePath {
    static constexpr StringView command_name = "StrokePath"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::StrokePath;

    Gfx::Path::CapStyle cap_style;
    Gfx::Path::JoinStyle join_style;
    float miter_limit;
    DisplayListDataSpan dash_array;
    float dash_offset;
    Gfx::IntRect path_bounding_rect;
    DisplayListDataSpan path_data;
    float opacity;
    PathPaintKind paint_kind { PathPaintKind::Color };
    Color color;
    PaintStyleResourceId paint_style_id;
    float thickness;
    ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void dump(StringBuilder&) const;
};

struct DrawEllipse {
    static constexpr StringView command_name = "DrawEllipse"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawEllipse;

    Gfx::IntRect rect;
    Color color;
    int thickness;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct FillEllipse {
    static constexpr StringView command_name = "FillEllipse"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::FillEllipse;

    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct DrawLine {
    static constexpr StringView command_name = "DrawLine"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawLine;

    Color color;
    Gfx::IntPoint from;
    Gfx::IntPoint to;
    int thickness;
    Gfx::LineStyle style;
    Color alternate_color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return Gfx::IntRect::from_two_points(from, to).inflated(thickness, thickness); }
    void dump(StringBuilder&) const;
};

struct ApplyBackdropFilter {
    static constexpr StringView command_name = "ApplyBackdropFilter"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::ApplyBackdropFilter;

    Gfx::IntRect backdrop_region;
    CornerRadii corner_radii;
    bool has_backdrop_filter { false };
    FilterResourceId backdrop_filter_id;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return backdrop_region; }

    void dump(StringBuilder&) const;
};

struct DrawRect {
    static constexpr StringView command_name = "DrawRect"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::DrawRect;

    Gfx::IntRect rect;
    Color color;
    bool rough;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintRadialGradient {
    static constexpr StringView command_name = "PaintRadialGradient"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintRadialGradient;

    Gfx::IntRect rect;
    DisplayListGradientColorStops color_stops;
    DisplayListColorInterpolationMethod interpolation_method;
    Gfx::IntPoint center;
    Gfx::IntSize size;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintConicGradient {
    static constexpr StringView command_name = "PaintConicGradient"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintConicGradient;

    Gfx::IntRect rect;
    float start_angle { 0.0f };
    DisplayListGradientColorStops color_stops;
    DisplayListColorInterpolationMethod interpolation_method;
    Gfx::IntPoint position;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct AddRoundedRectClip {
    static constexpr StringView command_name = "AddRoundedRectClip"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::AddRoundedRectClip;

    CornerRadii corner_radii;
    Gfx::IntRect border_rect;
    CornerClip corner_clip;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return border_rect; }
    bool is_clip() const { return true; }

    void dump(StringBuilder&) const;
};

struct PaintNestedDisplayList {
    static constexpr StringView command_name = "PaintNestedDisplayList"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintNestedDisplayList;

    DisplayListResourceId display_list_id;
    DisplayListDataSpan command_bytes;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintScrollBar {
    static constexpr StringView command_name = "PaintScrollBar"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::PaintScrollBar;

    ScrollFrameIndex scroll_frame_index;
    Gfx::IntRect gutter_rect;
    Gfx::IntRect thumb_rect;
    double scroll_size;
    Color thumb_color;
    Color track_color;
    bool vertical;

    void dump(StringBuilder&) const;
};

struct ApplyEffects {
    static constexpr StringView command_name = "ApplyEffects"sv;
    static constexpr DisplayListCommandType command_type = DisplayListCommandType::ApplyEffects;
    static constexpr int nesting_level_change = 1;

    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator { Gfx::CompositingAndBlendingOperator::Normal };
    bool has_filter { false };
    FilterResourceId filter_id;
    bool has_mask_kind { false };
    Gfx::MaskKind mask_kind {};

    void dump(StringBuilder&) const;
};

template<typename Command>
concept DisplayListCommand = requires {
    Command::command_type;
};

template<DisplayListCommand Command>
Command read_display_list_command_payload(ReadonlyBytes payload)
{
    VERIFY(payload.size() >= sizeof(Command));
    Command command;
    __builtin_memcpy(&command, payload.data(), sizeof(Command));
    return command;
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

template<DisplayListCommand Command>
consteval int display_list_command_nesting_level_change()
{
    if constexpr (requires { Command::nesting_level_change; })
        return Command::nesting_level_change;
    return 0;
}

inline int display_list_command_nesting_level_change(DisplayListCommandType command_type)
{
    return visit_display_list_command_type(command_type, []<DisplayListCommand Command>() {
        return display_list_command_nesting_level_change<Command>();
    });
}

static_assert(__is_trivially_copyable(DisplayListCommandHeader));

#define VERIFY_DISPLAY_LIST_COMMAND(command, player_method) static_assert(IsTriviallyDestructible<command>);
ENUMERATE_DISPLAY_LIST_COMMANDS(VERIFY_DISPLAY_LIST_COMMAND)
#undef VERIFY_DISPLAY_LIST_COMMAND

}
