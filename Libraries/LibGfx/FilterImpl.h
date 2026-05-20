/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Filter.h>

namespace Gfx {

struct FilterImpl {
    struct Arithmetic {
        Optional<Filter> background;
        Optional<Filter> foreground;
        float k1 { 0.0f };
        float k2 { 0.0f };
        float k3 { 0.0f };
        float k4 { 0.0f };
    };

    struct Compose {
        Filter outer;
        Filter inner;
    };

    struct Blend {
        Optional<Filter> background;
        Optional<Filter> foreground;
        Gfx::CompositingAndBlendingOperator mode { Gfx::CompositingAndBlendingOperator::Normal };
    };

    struct Flood {
        Gfx::Color color;
        float opacity { 1.0f };
    };

    struct DisplacementMap {
        Optional<Filter> color;
        Optional<Filter> displacement;
        float scale { 0.0f };
        ChannelSelector x_channel_selector { ChannelSelector::Alpha };
        ChannelSelector y_channel_selector { ChannelSelector::Alpha };
    };

    struct DropShadow {
        float offset_x { 0.0f };
        float offset_y { 0.0f };
        float radius { 0.0f };
        Gfx::Color color;
        Optional<Filter> input;
    };

    struct Blur {
        float radius_x { 0.0f };
        float radius_y { 0.0f };
        Optional<Filter> input;
    };

    struct ColorFilter {
        ColorFilterType type { ColorFilterType::Brightness };
        float amount { 0.0f };
        Optional<Filter> input;
    };

    struct ColorMatrix {
        Array<float, 20> matrix;
        Optional<Filter> input;
    };

    struct ColorTable {
        Optional<ByteBuffer> a;
        Optional<ByteBuffer> r;
        Optional<ByteBuffer> g;
        Optional<ByteBuffer> b;
        Optional<Filter> input;
    };

    struct Saturate {
        float value { 0.0f };
        Optional<Filter> input;
    };

    struct HueRotate {
        float angle_degrees { 0.0f };
        Optional<Filter> input;
    };

    struct Image {
        Gfx::DecodedImageFrame frame;
        Gfx::IntRect src_rect;
        Gfx::IntRect dest_rect;
        Gfx::ScalingMode scaling_mode { Gfx::ScalingMode::NearestNeighbor };
    };

    struct Merge {
        Vector<Optional<Filter>> inputs;
    };

    struct Offset {
        float dx { 0.0f };
        float dy { 0.0f };
        Optional<Filter> input;
    };

    struct Erode {
        float radius_x { 0.0f };
        float radius_y { 0.0f };
        Optional<Filter> input;
    };

    struct Dilate {
        float radius_x { 0.0f };
        float radius_y { 0.0f };
        Optional<Filter> input;
    };

    struct Turbulence {
        TurbulenceType turbulence_type { TurbulenceType::Turbulence };
        float base_frequency_x { 0.0f };
        float base_frequency_y { 0.0f };
        i32 num_octaves { 0 };
        float seed { 0.0f };
        Gfx::IntSize tile_stitch_size;
    };

    using Operation = Variant<
        Arithmetic,
        Compose,
        Blend,
        Flood,
        DisplacementMap,
        DropShadow,
        Blur,
        ColorFilter,
        ColorMatrix,
        ColorTable,
        Saturate,
        HueRotate,
        Image,
        Merge,
        Offset,
        Erode,
        Dilate,
        Turbulence>;

    enum class OperationType : u8 {
        Arithmetic,
        Compose,
        Blend,
        Flood,
        DisplacementMap,
        DropShadow,
        Blur,
        ColorFilter,
        ColorMatrix,
        ColorTable,
        Saturate,
        HueRotate,
        Image,
        Merge,
        Offset,
        Erode,
        Dilate,
        Turbulence,
    };

    Operation operation;

    static NonnullOwnPtr<FilterImpl> create(Operation operation)
    {
        return adopt_own(*new FilterImpl(move(operation)));
    }

    NonnullOwnPtr<FilterImpl> clone() const
    {
        return adopt_own(*new FilterImpl(operation));
    }
};

}
