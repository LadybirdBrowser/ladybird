/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/InterpolationColorSpace.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibIPC/Forward.h>

namespace Gfx {

enum class ColorFilterType {
    Brightness,
    Contrast,
    Grayscale,
    Invert,
    Opacity,
    Saturate,
    Sepia
};

// A decoded image frame a filter graph draws, under the id the graph's bytes name it by.
struct FilterImageFrame {
    u64 id { 0 };
    DecodedImageFrame frame;
};

// An image filter graph in the serialized form painting hands across the FFI, the display list and
// IPC. The image frames the graph draws travel alongside the bytes, so a consumer without a display
// list resource storage can still find their pixels.
class Filter {
public:
    Filter(ByteBuffer serialized_bytes, Vector<FilterImageFrame> image_frames);

    // The filter functions of a CSS filter list. SVG filter primitives are built on the Rust side.
    static Filter compose(Filter const& outer, Filter const& inner);
    static Filter drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input = {});
    static Filter blur(float radius_x, float radius_y, Optional<Filter const&> input = {});
    static Filter color(ColorFilterType type, float amount, Optional<Filter const&> input = {});
    static Filter hue_rotate(float angle_degrees, Optional<Filter const&> input = {});

    ReadonlyBytes serialized_bytes() const { return m_serialized_bytes; }
    ReadonlySpan<FilterImageFrame> image_frames() const { return m_image_frames; }
    DecodedImageFrame const& image_frame(u64 id) const;

private:
    ByteBuffer m_serialized_bytes;
    Vector<FilterImageFrame> m_image_frames;
};

// Visits the id of every image frame a serialized filter graph draws. Bytes that do not hold a
// filter graph visit nothing.
void for_each_filter_image_frame_id(ReadonlyBytes serialized_filter, Function<void(u64)> const&);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::Filter const&);

template<>
ErrorOr<Gfx::Filter> decode(Decoder&);

}
