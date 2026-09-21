/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Filter.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

struct EasingFunction;

}

// The description of a compositor-driven animation the main thread builds from an effect's keyframes. The visual
// context tree takes it over and samples it in Rust; what remains here is what the main thread compares and validates
// before handing it off.
namespace Web::Compositor {

struct VisualAnimationEasing {
    enum class Kind : u8 {
        Linear,
        CubicBezier,
        Steps,
    };

    struct LinearPoint {
        double input { 0 };
        double output { 0 };

        bool operator==(LinearPoint const&) const = default;
    };

    static WEB_API VisualAnimationEasing from_css(CSS::EasingFunction const&);

    Kind kind { Kind::Linear };
    Vector<LinearPoint> linear_points { { 0, 0 }, { 1, 1 } };
    double x1 { 0 };
    double y1 { 0 };
    double x2 { 1 };
    double y2 { 1 };
    i32 interval_count { 1 };
    u8 step_position { 0 };

    bool operator==(VisualAnimationEasing const&) const = default;
};

enum class VisualAnimationPlaybackDirection : u8 {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
};

enum class VisualAnimationFillMode : u8 {
    None,
    Backwards,
};

enum class VisualAnimationTransformOperationKind : u8 {
    Translate,
    Translate3d,
    TranslateX,
    TranslateY,
    TranslateZ,
    Scale,
    Scale3d,
    ScaleX,
    ScaleY,
    ScaleZ,
    Rotate,
    RotateX,
    RotateY,
    RotateZ,
    Skew,
    SkewX,
    SkewY,
};

enum class VisualAnimationFilterOperationKind : u8 {
    Blur,
    DropShadow,
    Color,
    HueRotate,
};

struct VisualAnimationFilterOperation {
    VisualAnimationFilterOperationKind kind { VisualAnimationFilterOperationKind::Blur };
    float amount { 0 };
    float offset_x { 0 };
    float offset_y { 0 };
    Gfx::Color color { Gfx::Color::Transparent };
    Gfx::ColorFilterType color_operation { Gfx::ColorFilterType::Brightness };

    bool operator==(VisualAnimationFilterOperation const&) const = default;
};

struct VisualAnimationTransformOperation {
    // Whether the values are a finite list of the length the kind takes.
    WEB_API bool is_valid() const;

    VisualAnimationTransformOperationKind kind { VisualAnimationTransformOperationKind::Translate };
    // Translation lengths are expressed in device pixels, matching TransformData matrices.
    Vector<float> values;

    bool operator==(VisualAnimationTransformOperation const&) const = default;
};

using VisualAnimationTransformList = Vector<VisualAnimationTransformOperation>;
using VisualAnimationFilterList = Vector<VisualAnimationFilterOperation>;
using VisualAnimationValue = Variant<float, Gfx::Color, VisualAnimationFilterList, VisualAnimationTransformList>;

struct VisualAnimationKeyframe {
    double offset { 0 };
    VisualAnimationEasing easing;
    VisualAnimationValue value;

    bool operator==(VisualAnimationKeyframe const&) const = default;
};

struct VisualAnimation {
    enum class TargetKind : u8 {
        Opacity,
        BackgroundColor,
        Filter,
        Transform,
    };

    WEB_API bool is_valid() const;
    // Whether the timing and keyframes describe an animation the tree can sample, whatever nodes it names.
    WEB_API bool has_valid_animation_parameters() const;
    WEB_API bool has_same_animation_parameters(VisualAnimation const&) const;
    WEB_API bool has_same_parameters_except_anchor(VisualAnimation const&) const;

    TargetKind target_kind { TargetKind::Opacity };
    Vector<u32> visual_context_node_indices;
    i64 monotonic_time_at_anchor_ns { 0 };
    double local_time_at_anchor_ms { 0 };
    double playback_rate { 1 };
    double start_delay_ms { 0 };
    double iteration_duration_ms { 0 };
    double iteration_count { AK::Infinity<double> };
    double iteration_start { 0 };
    VisualAnimationPlaybackDirection playback_direction { VisualAnimationPlaybackDirection::Normal };
    VisualAnimationFillMode fill_mode { VisualAnimationFillMode::None };
    VisualAnimationEasing easing;
    Vector<VisualAnimationKeyframe> keyframes;

    bool operator==(VisualAnimation const&) const = default;
};

}
