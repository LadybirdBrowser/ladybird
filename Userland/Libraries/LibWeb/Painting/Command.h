/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/SegmentedVector.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Color.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

class DisplayList;

struct DrawGlyphRun {
    NonnullRefPtr<Gfx::GlyphRun> glyph_run;
    Color color;
    Gfx::IntRect rect;
    Gfx::FloatPoint translation;
    double scale { 1 };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset);
};

struct FillRect {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct DrawScaledBitmap {
    Gfx::IntRect dst_rect;
    NonnullRefPtr<Gfx::Bitmap> bitmap;
    Gfx::IntRect src_rect;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void translate_by(Gfx::IntPoint const& offset) { dst_rect.translate_by(offset); }
};

struct DrawScaledImmutableBitmap {
    Gfx::IntRect dst_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap;
    Gfx::IntRect src_rect;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
    void translate_by(Gfx::IntPoint const& offset) { dst_rect.translate_by(offset); }
};

struct DrawRepeatedImmutableBitmap {
    struct Repeat {
        bool x { false };
        bool y { false };
    };

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap;
    Gfx::ScalingMode scaling_mode;
    Repeat repeat;

    void translate_by(Gfx::IntPoint const& offset) { dst_rect.translate_by(offset); }
};

struct Save { };
struct Restore { };

struct AddClipRect {
    Gfx::IntRect rect;

    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct StackingContextTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;
};

struct PushStackingContext {
    float opacity;
    // The bounding box of the source paintable (pre-transform).
    Gfx::IntRect source_paintable_rect;
    // A translation to be applied after the stacking context has been transformed.
    Gfx::IntPoint post_transform_translation;
    StackingContextTransform transform;
    Optional<Gfx::Path> clip_path = {};

    void translate_by(Gfx::IntPoint const& offset)
    {
        source_paintable_rect.translate_by(offset);
        transform.origin.translate_by(offset.to_type<float>());
    }
};

struct PopStackingContext { };

struct PaintLinearGradient {
    Gfx::IntRect gradient_rect;
    LinearGradientData linear_gradient_data;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return gradient_rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        gradient_rect.translate_by(offset);
    }
};

struct PaintOuterBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void translate_by(Gfx::IntPoint const& offset);
};

struct PaintInnerBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
    void translate_by(Gfx::IntPoint const& offset);
};

struct PaintTextShadow {
    int blur_radius;
    Gfx::IntRect shadow_bounding_rect;
    Gfx::IntRect text_rect;
    NonnullRefPtr<Gfx::GlyphRun> glyph_run;
    double glyph_run_scale { 1 };
    Color color;
    Gfx::IntPoint draw_location;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return { draw_location, shadow_bounding_rect.size() }; }
    void translate_by(Gfx::IntPoint const& offset) { draw_location.translate_by(offset); }
};

struct FillRectWithRoundedCorners {
    Gfx::IntRect rect;
    Color color;
    CornerRadii corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct FillPathUsingColor {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    Color color;
    Gfx::WindingRule winding_rule;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        path_bounding_rect.translate_by(offset);
        aa_translation.translate_by(offset.to_type<float>());
    }
};

struct FillPathUsingPaintStyle {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    PaintStyle paint_style;
    Gfx::WindingRule winding_rule;
    float opacity;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        path_bounding_rect.translate_by(offset);
        aa_translation.translate_by(offset.to_type<float>());
    }
};

struct StrokePathUsingColor {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    Color color;
    float thickness;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        path_bounding_rect.translate_by(offset);
        aa_translation.translate_by(offset.to_type<float>());
    }
};

struct StrokePathUsingPaintStyle {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    PaintStyle paint_style;
    float thickness;
    float opacity = 1.0f;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        path_bounding_rect.translate_by(offset);
        aa_translation.translate_by(offset.to_type<float>());
    }
};

struct DrawEllipse {
    Gfx::IntRect rect;
    Color color;
    int thickness;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        rect.translate_by(offset);
    }
};

struct FillEllipse {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        rect.translate_by(offset);
    }
};

struct DrawLine {
    Color color;
    Gfx::IntPoint from;
    Gfx::IntPoint to;
    int thickness;
    Gfx::LineStyle style;
    Color alternate_color;

    void translate_by(Gfx::IntPoint const& offset)
    {
        from.translate_by(offset);
        to.translate_by(offset);
    }
};

struct ApplyBackdropFilter {
    Gfx::IntRect backdrop_region;
    BorderRadiiData border_radii_data;
    CSS::ResolvedBackdropFilter backdrop_filter;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return backdrop_region; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        backdrop_region.translate_by(offset);
    }
};

struct DrawRect {
    Gfx::IntRect rect;
    Color color;
    bool rough;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct PaintRadialGradient {
    Gfx::IntRect rect;
    RadialGradientData radial_gradient_data;
    Gfx::IntPoint center;
    Gfx::IntSize size;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct PaintConicGradient {
    Gfx::IntRect rect;
    ConicGradientData conic_gradient_data;
    Gfx::IntPoint position;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset) { rect.translate_by(offset); }
};

struct DrawTriangleWave {
    Gfx::IntPoint p1;
    Gfx::IntPoint p2;
    Color color;
    int amplitude;
    int thickness;

    void translate_by(Gfx::IntPoint const& offset)
    {
        p1.translate_by(offset);
        p2.translate_by(offset);
    }
};

struct AddRoundedRectClip {
    CornerRadii corner_radii;
    Gfx::IntRect border_rect;
    CornerClip corner_clip;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return border_rect; }

    void translate_by(Gfx::IntPoint const& offset) { border_rect.translate_by(offset); }
};

struct AddMask {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        rect.translate_by(offset);
    }
};

struct PaintNestedDisplayList {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }

    void translate_by(Gfx::IntPoint const& offset)
    {
        rect.translate_by(offset);
    }
};

struct PaintScrollBar {
    int scroll_frame_id;
    Gfx::IntRect rect;
    CSSPixelFraction scroll_size;
    bool vertical;

    void translate_by(Gfx::IntPoint const& offset)
    {
        rect.translate_by(offset);
    }
};

struct ApplyOpacity {
    float opacity;
};

struct ApplyTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;
    Gfx::IntPoint post_transform_translation;

    void translate_by(Gfx::IntPoint const& offset)
    {
        origin.translate_by(offset.to_type<float>());
    }
};

struct ApplyMaskBitmap {
    Gfx::IntPoint origin;
    NonnullRefPtr<Gfx::Bitmap> bitmap;
    Gfx::Bitmap::MaskKind kind;

    void translate_by(Gfx::IntPoint const& offset)
    {
        origin.translate_by(offset);
    }
};

using Command = Variant<
    DrawGlyphRun,
    FillRect,
    DrawScaledBitmap,
    DrawScaledImmutableBitmap,
    DrawRepeatedImmutableBitmap,
    Save,
    Restore,
    AddClipRect,
    PushStackingContext,
    PopStackingContext,
    PaintLinearGradient,
    PaintRadialGradient,
    PaintConicGradient,
    PaintOuterBoxShadow,
    PaintInnerBoxShadow,
    PaintTextShadow,
    FillRectWithRoundedCorners,
    FillPathUsingColor,
    FillPathUsingPaintStyle,
    StrokePathUsingColor,
    StrokePathUsingPaintStyle,
    DrawEllipse,
    FillEllipse,
    DrawLine,
    ApplyBackdropFilter,
    DrawRect,
    DrawTriangleWave,
    AddRoundedRectClip,
    AddMask,
    PaintNestedDisplayList,
    PaintScrollBar,
    ApplyOpacity,
    ApplyTransform,
    ApplyMaskBitmap>;

}
