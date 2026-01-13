/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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
#include <LibGfx/PaintingSurface.h>
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
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ShouldAntiAlias.h>

namespace Web::Painting {

class DisplayList;

struct DrawGlyphRun {
    NonnullRefPtr<Gfx::GlyphRun const> glyph_run;
    double scale { 1 };
    Gfx::IntRect rect;
    Gfx::FloatPoint translation;
    Color color;
    Gfx::Orientation orientation { Gfx::Orientation::Horizontal };
    Gfx::IntRect bounding_rectangle;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return bounding_rectangle; }
    void dump(StringBuilder&) const;
};

struct FillRect {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

struct DrawPaintingSurface {
    Gfx::IntRect dst_rect;
    NonnullRefPtr<Gfx::PaintingSurface const> surface;
    Gfx::IntRect src_rect;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void dump(StringBuilder&) const;
};

struct DrawScaledImmutableBitmap {
    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
    void dump(StringBuilder&) const;
};

struct DrawRepeatedImmutableBitmap {
    struct Repeat {
        bool x { false };
        bool y { false };
    };

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap;
    Gfx::ScalingMode scaling_mode;
    Repeat repeat;

    void dump(StringBuilder&) const;
};

struct Save {
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct SaveLayer {
    static constexpr int nesting_level_change = 1;

    void dump(StringBuilder&) const;
};

struct Restore {
    static constexpr int nesting_level_change = -1;

    void dump(StringBuilder&) const;
};

struct Translate {
    Gfx::IntPoint delta;

    void dump(StringBuilder&) const;
};

struct AddClipRect {
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip_or_mask() const { return true; }
    void dump(StringBuilder&) const;
};

struct AddClipPath {
    Gfx::Path path;
    Gfx::IntRect bounding_rectangle;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return bounding_rectangle; }
    bool is_clip_or_mask() const { return true; }
    void dump(StringBuilder&) const;
};

struct PushStackingContext {
    static constexpr int nesting_level_change = 1;

    float opacity;
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator;
    bool isolate;
    Optional<Gfx::Path> clip_path = {};

    size_t matching_pop_index { 0 };
    bool can_aggregate_children_bounds { false };
    Optional<Gfx::IntRect> bounding_rect {};

    void dump(StringBuilder&) const;
};

struct PopStackingContext {
    static constexpr int nesting_level_change = -1;

    void dump(StringBuilder&) const;
};

struct PaintLinearGradient {
    Gfx::IntRect gradient_rect;
    LinearGradientData linear_gradient_data;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return gradient_rect; }

    void dump(StringBuilder&) const;
};

struct PaintOuterBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintInnerBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void dump(StringBuilder&) const;
};

struct PaintTextShadow {
    NonnullRefPtr<Gfx::GlyphRun const> glyph_run;
    double glyph_run_scale { 1 };
    Gfx::IntRect shadow_bounding_rect;
    Gfx::IntRect text_rect;
    Gfx::FloatPoint draw_location;
    int blur_radius;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return { draw_location.to_type<int>(), shadow_bounding_rect.size() }; }
    void dump(StringBuilder&) const;
};

struct FillRectWithRoundedCorners {
    Gfx::IntRect rect;
    Color color;
    CornerRadii corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void dump(StringBuilder&) const;
};

struct FillPath {
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
    Gfx::IntRect rect;
    Color color;
    int thickness;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct FillEllipse {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct DrawLine {
    Color color;
    Gfx::IntPoint from;
    Gfx::IntPoint to;
    int thickness;
    Gfx::LineStyle style;
    Color alternate_color;

    void dump(StringBuilder&) const;
};

struct ApplyBackdropFilter {
    Gfx::IntRect backdrop_region;
    BorderRadiiData border_radii_data;
    Optional<Gfx::Filter> backdrop_filter;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return backdrop_region; }

    void dump(StringBuilder&) const;
};

struct DrawRect {
    Gfx::IntRect rect;
    Color color;
    bool rough;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintRadialGradient {
    Gfx::IntRect rect;
    RadialGradientData radial_gradient_data;
    Gfx::IntPoint center;
    Gfx::IntSize size;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintConicGradient {
    Gfx::IntRect rect;
    ConicGradientData conic_gradient_data;
    Gfx::IntPoint position;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct AddRoundedRectClip {
    CornerRadii corner_radii;
    Gfx::IntRect border_rect;
    CornerClip corner_clip;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return border_rect; }
    bool is_clip_or_mask() const { return true; }

    void dump(StringBuilder&) const;
};

struct AddMask {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip_or_mask() const { return true; }

    void dump(StringBuilder&) const;
};

struct PaintNestedDisplayList {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void dump(StringBuilder&) const;
};

struct PaintScrollBar {
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
    static constexpr int nesting_level_change = 1;

    float opacity { 1.0f };
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator { Gfx::CompositingAndBlendingOperator::Normal };
    Optional<Gfx::Filter> filter {};

    void dump(StringBuilder&) const;
};

struct ApplyTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;

    void dump(StringBuilder&) const;
};

struct ApplyMaskBitmap {
    Gfx::IntPoint origin;
    NonnullRefPtr<Gfx::ImmutableBitmap const> bitmap;
    Gfx::MaskKind kind;

    void dump(StringBuilder&) const;
};

using DisplayListCommand = Variant<
    DrawGlyphRun,
    FillRect,
    DrawPaintingSurface,
    DrawScaledImmutableBitmap,
    DrawRepeatedImmutableBitmap,
    Save,
    SaveLayer,
    Restore,
    Translate,
    AddClipRect,
    AddClipPath,
    PushStackingContext,
    PopStackingContext,
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
    AddMask,
    PaintNestedDisplayList,
    PaintScrollBar,
    ApplyEffects,
    ApplyTransform,
    ApplyMaskBitmap>;

}
