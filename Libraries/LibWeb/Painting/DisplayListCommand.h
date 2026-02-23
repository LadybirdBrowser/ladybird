/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ShouldAntiAlias.h>

namespace Web::Painting {

class DisplayList;

struct DrawGlyphRun {
    static constexpr StringView command_name = "DrawGlyphRun"sv;

    NonnullRefPtr<Gfx::GlyphRun const> glyph_run;
    Gfx::IntRect rect;
    Gfx::FloatPoint translation;
    Color color;
    Gfx::Orientation orientation { Gfx::Orientation::Horizontal };

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct FillRect {
    static constexpr StringView command_name = "FillRect"sv;

    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

struct DrawScaledImmutableBitmap {
    static constexpr StringView command_name = "DrawScaledImmutableBitmap"sv;

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
    void dump(StringBuilder&) const;
};

struct DrawRepeatedImmutableBitmap {
    static constexpr StringView command_name = "DrawRepeatedImmutableBitmap"sv;

    struct Repeat {
        bool x { false };
        bool y { false };
    };

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap;
    Gfx::ScalingMode scaling_mode;
    Repeat repeat;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
    void dump(StringBuilder&) const;
};

struct DrawExternalContent {
    static constexpr StringView command_name = "DrawExternalContent"sv;

    Gfx::IntRect dst_rect;
    NonnullRefPtr<ExternalContentSource> source;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void dump(StringBuilder&) const;
};

struct Save {
    static constexpr StringView command_name = "Save"sv;
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct SaveLayer {
    static constexpr StringView command_name = "SaveLayer"sv;
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct Restore {
    static constexpr StringView command_name = "Restore"sv;
    static constexpr int nesting_level_change = -1;

    void dump(StringBuilder&) const;
};

struct Translate {
    static constexpr StringView command_name = "Translate"sv;

    Gfx::IntPoint delta;

    void dump(StringBuilder&) const;
};

struct AddClipRect {
    static constexpr StringView command_name = "AddClipRect"sv;

    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip_or_mask() const { return true; }
    void dump(StringBuilder&) const;
};

struct PaintLinearGradient {
    static constexpr StringView command_name = "PaintLinearGradient"sv;

    Gfx::IntRect gradient_rect;
    LinearGradientData linear_gradient_data;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return gradient_rect; }

    void dump(StringBuilder&) const;
};

struct PaintOuterBoxShadow {
    static constexpr StringView command_name = "PaintOuterBoxShadow"sv;

    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintInnerBoxShadow {
    static constexpr StringView command_name = "PaintInnerBoxShadow"sv;

    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintTextShadow {
    static constexpr StringView command_name = "PaintTextShadow"sv;

    NonnullRefPtr<Gfx::GlyphRun const> glyph_run;
    Gfx::IntRect shadow_bounding_rect;
    Gfx::IntRect text_rect;
    Gfx::FloatPoint draw_location;
    int blur_radius;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return { draw_location.to_type<int>(), shadow_bounding_rect.size() }; }
    void dump(StringBuilder&) const;
};

struct FillRectWithRoundedCorners {
    static constexpr StringView command_name = "FillRectWithRoundedCorners"sv;

    Gfx::IntRect rect;
    Color color;
    CornerRadii corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

struct FillPath {
    static constexpr StringView command_name = "FillPath"sv;

    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    float opacity { 1.0f };
    PaintStyleOrColor paint_style_or_color;
    Gfx::WindingRule winding_rule;
    ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void dump(StringBuilder&) const;
};

struct StrokePath {
    static constexpr StringView command_name = "StrokePath"sv;

    Gfx::Path::CapStyle cap_style;
    Gfx::Path::JoinStyle join_style;
    float miter_limit;
    Vector<float> dash_array;
    float dash_offset;
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    float opacity;
    PaintStyleOrColor paint_style_or_color;
    float thickness;
    ShouldAntiAlias should_anti_alias { ShouldAntiAlias::Yes };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void dump(StringBuilder&) const;
};

struct DrawEllipse {
    static constexpr StringView command_name = "DrawEllipse"sv;

    Gfx::IntRect rect;
    Color color;
    int thickness;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct FillEllipse {
    static constexpr StringView command_name = "FillEllipse"sv;

    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct DrawLine {
    static constexpr StringView command_name = "DrawLine"sv;

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

    Gfx::IntRect backdrop_region;
    BorderRadiiData border_radii_data;
    Optional<Gfx::Filter> backdrop_filter;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return backdrop_region; }

    void dump(StringBuilder&) const;
};

struct DrawRect {
    static constexpr StringView command_name = "DrawRect"sv;

    Gfx::IntRect rect;
    Color color;
    bool rough;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintRadialGradient {
    static constexpr StringView command_name = "PaintRadialGradient"sv;

    Gfx::IntRect rect;
    RadialGradientData radial_gradient_data;
    Gfx::IntPoint center;
    Gfx::IntSize size;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintConicGradient {
    static constexpr StringView command_name = "PaintConicGradient"sv;

    Gfx::IntRect rect;
    ConicGradientData conic_gradient_data;
    Gfx::IntPoint position;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct AddRoundedRectClip {
    static constexpr StringView command_name = "AddRoundedRectClip"sv;

    CornerRadii corner_radii;
    Gfx::IntRect border_rect;
    CornerClip corner_clip;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return border_rect; }
    bool is_clip_or_mask() const { return true; }

    void dump(StringBuilder&) const;
};

struct PaintNestedDisplayList {
    static constexpr StringView command_name = "PaintNestedDisplayList"sv;

    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintScrollBar {
    static constexpr StringView command_name = "PaintScrollBar"sv;

    int scroll_frame_id { 0 };
    Gfx::IntRect gutter_rect;
    Gfx::IntRect thumb_rect;
    CSSPixelFraction scroll_size;
    Color thumb_color;
    Color track_color;
    bool vertical;

    void dump(StringBuilder&) const;
};

struct ApplyEffects {
    static constexpr StringView command_name = "ApplyEffects"sv;
    static constexpr int nesting_level_change = 1;

    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator { Gfx::CompositingAndBlendingOperator::Normal };
    Optional<Gfx::Filter> filter {};
    Optional<Gfx::MaskKind> mask_kind {};

    void dump(StringBuilder&) const;
};

using DisplayListCommand = Variant<
    DrawGlyphRun,
    FillRect,
    DrawScaledImmutableBitmap,
    DrawRepeatedImmutableBitmap,
    DrawExternalContent,
    Save,
    SaveLayer,
    Restore,
    Translate,
    AddClipRect,
    PaintLinearGradient,
    PaintRadialGradient,
    PaintConicGradient,
    PaintOuterBoxShadow,
    PaintInnerBoxShadow,
    PaintTextShadow,
    FillRectWithRoundedCorners,
    FillPath,
    StrokePath,
    DrawEllipse,
    FillEllipse,
    DrawLine,
    ApplyBackdropFilter,
    DrawRect,
    AddRoundedRectClip,
    PaintNestedDisplayList,
    PaintScrollBar,
    ApplyEffects>;

}
