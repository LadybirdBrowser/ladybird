/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <RustFFI.h>

namespace Gfx {

namespace {

// Writes one node of a filter graph in the layout libgfx_rust's filter codec reads: the operation
// tag, then the operation's fields in declaration order, with an optional input as a presence byte
// followed by the input's own bytes.
class FilterWriter {
public:
    explicit FilterWriter(FFI::FilterOperationType type)
    {
        m_bytes.append(to_underlying(type));
    }

    template<typename T>
    requires(Traits<T>::is_trivially_serializable())
    void value(T value)
    {
        m_bytes.append(&value, sizeof(value));
    }

    void boolean(bool value)
    {
        m_bytes.append(static_cast<u8>(value));
    }

    void color(Color color)
    {
        value<u32>(color.value());
    }

    void rect(IntRect const& rect)
    {
        value<i32>(rect.x());
        value<i32>(rect.y());
        value<i32>(rect.width());
        value<i32>(rect.height());
    }

    void size(IntSize const& size)
    {
        value<i32>(size.width());
        value<i32>(size.height());
    }

    void optional_color_table(Optional<ReadonlyBytes> table)
    {
        boolean(table.has_value());
        if (!table.has_value())
            return;
        VERIFY(table->size() == 256);
        value<u32>(table->size());
        m_bytes.append(*table);
    }

    void filter(Filter const& filter)
    {
        m_bytes.append(filter.serialized_bytes());
        for (auto const& image_frame : filter.image_frames())
            m_image_frames.append(image_frame);
    }

    void optional_filter(Optional<Filter const&> filter)
    {
        boolean(filter.has_value());
        if (filter.has_value())
            this->filter(*filter);
    }

    void image_frame(DecodedImageFrame const& frame)
    {
        value<u64>(frame.id());
        m_image_frames.append({ frame.id(), frame });
    }

    Filter finish()
    {
        return Filter { move(m_bytes), move(m_image_frames) };
    }

private:
    ByteBuffer m_bytes;
    Vector<FilterImageFrame> m_image_frames;
};

}

Filter::Filter(ByteBuffer serialized_bytes, Vector<FilterImageFrame> image_frames)
    : m_serialized_bytes(move(serialized_bytes))
    , m_image_frames(move(image_frames))
{
}

DecodedImageFrame const& Filter::image_frame(u64 id) const
{
    for (auto const& image_frame : m_image_frames) {
        if (image_frame.id == id)
            return image_frame.frame;
    }
    VERIFY_NOT_REACHED();
}

Filter Filter::arithmetic(Optional<Filter const&> background, Optional<Filter const&> foreground, float k1, float k2, float k3, float k4)
{
    FilterWriter writer { FFI::FilterOperationType::Arithmetic };
    writer.optional_filter(background);
    writer.optional_filter(foreground);
    writer.value(k1);
    writer.value(k2);
    writer.value(k3);
    writer.value(k4);
    return writer.finish();
}

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    FilterWriter writer { FFI::FilterOperationType::Compose };
    writer.filter(outer);
    writer.filter(inner);
    return writer.finish();
}

Filter Filter::blend(Optional<Filter const&> background, Optional<Filter const&> foreground, CompositingAndBlendingOperator mode)
{
    FilterWriter writer { FFI::FilterOperationType::Blend };
    writer.optional_filter(background);
    writer.optional_filter(foreground);
    writer.value(mode);
    return writer.finish();
}

Filter Filter::flood(Gfx::Color color, float opacity)
{
    FilterWriter writer { FFI::FilterOperationType::Flood };
    writer.color(color);
    writer.value(opacity);
    return writer.finish();
}

Filter Filter::displacement_map(Optional<Filter const&> color, Optional<Filter const&> displacement, float scale, ChannelSelector x_channel_selector, ChannelSelector y_channel_selector)
{
    FilterWriter writer { FFI::FilterOperationType::DisplacementMap };
    writer.optional_filter(color);
    writer.optional_filter(displacement);
    writer.value(scale);
    writer.value(x_channel_selector);
    writer.value(y_channel_selector);
    return writer.finish();
}

Filter Filter::drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::DropShadow };
    writer.value(offset_x);
    writer.value(offset_y);
    writer.value(radius);
    writer.color(color);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::blur(float radius_x, float radius_y, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::Blur };
    writer.value(radius_x);
    writer.value(radius_y);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::color(ColorFilterType type, float amount, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::ColorFilter };
    writer.value(type);
    writer.value(amount);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::color_matrix(float matrix[20], Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::ColorMatrix };
    for (size_t i = 0; i < 20; ++i)
        writer.value(matrix[i]);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::color_table(Optional<ReadonlyBytes> a, Optional<ReadonlyBytes> r, Optional<ReadonlyBytes> g, Optional<ReadonlyBytes> b, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::ColorTable };
    writer.optional_color_table(a);
    writer.optional_color_table(r);
    writer.optional_color_table(g);
    writer.optional_color_table(b);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::saturate(float value, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::Saturate };
    writer.value(value);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::hue_rotate(float angle_degrees, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::HueRotate };
    writer.value(angle_degrees);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::image(Gfx::DecodedImageFrame const& frame, Gfx::IntRect const& src_rect, Gfx::IntRect const& dest_rect, Gfx::ScalingMode scaling_mode)
{
    FilterWriter writer { FFI::FilterOperationType::Image };
    writer.image_frame(frame);
    writer.rect(src_rect);
    writer.rect(dest_rect);
    writer.value(scaling_mode);
    return writer.finish();
}

Filter Filter::merge(Vector<Optional<Filter>> const& inputs)
{
    FilterWriter writer { FFI::FilterOperationType::Merge };
    VERIFY(inputs.size() <= NumericLimits<u32>::max());
    writer.value<u32>(inputs.size());
    for (auto const& input : inputs)
        writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::offset(float dx, float dy, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::Offset };
    writer.value(dx);
    writer.value(dy);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::erode(float radius_x, float radius_y, Optional<Filter> const& input)
{
    FilterWriter writer { FFI::FilterOperationType::Erode };
    writer.value(radius_x);
    writer.value(radius_y);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::dilate(float radius_x, float radius_y, Optional<Filter> const& input)
{
    FilterWriter writer { FFI::FilterOperationType::Dilate };
    writer.value(radius_x);
    writer.value(radius_y);
    writer.optional_filter(input);
    return writer.finish();
}

Filter Filter::turbulence(TurbulenceType turbulence_type, float base_frequency_x, float base_frequency_y, i32 num_octaves, float seed, Gfx::IntSize const& tile_stitch_size)
{
    FilterWriter writer { FFI::FilterOperationType::Turbulence };
    writer.value(turbulence_type);
    writer.value(base_frequency_x);
    writer.value(base_frequency_y);
    writer.value(num_octaves);
    writer.value(seed);
    writer.size(tile_stitch_size);
    return writer.finish();
}

Filter Filter::convert_interpolation_color_space(InterpolationColorSpace source_color_space, InterpolationColorSpace destination_color_space, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::ColorSpaceConversion };
    writer.value(source_color_space);
    writer.value(destination_color_space);
    writer.optional_filter(input);
    return writer.finish();
}

void for_each_filter_image_frame_id(ReadonlyBytes serialized_filter, Function<void(u64)> const& visit)
{
    FFI::ladybird_gfx_filter_for_each_image_frame_id(
        serialized_filter.data(),
        serialized_filter.size(),
        [](void const* context, u64 image_frame_id) {
            (*static_cast<Function<void(u64)> const*>(context))(image_frame_id);
        },
        &visit);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Filter const& filter)
{
    TRY(encoder.encode(filter.serialized_bytes()));
    TRY(encoder.encode_size(filter.image_frames().size()));
    for (auto const& image_frame : filter.image_frames()) {
        auto bitmap = image_frame.frame.bitmap().to_shareable_bitmap();
        if (!bitmap.is_valid())
            return Error::from_string_literal("IPC encode: failed to create shareable bitmap for filter image");
        TRY(encoder.encode(image_frame.id));
        TRY(encoder.encode(bitmap));
        TRY(encoder.encode(image_frame.frame.color_space()));
    }
    return {};
}

template<>
ErrorOr<Gfx::Filter> decode(Decoder& decoder)
{
    auto serialized_bytes = TRY(decoder.decode<ByteBuffer>());
    auto image_frame_count = TRY(decoder.decode_size());
    Vector<Gfx::FilterImageFrame> image_frames;
    TRY(image_frames.try_ensure_capacity(image_frame_count));
    for (size_t i = 0; i < image_frame_count; ++i) {
        auto id = TRY(decoder.decode<u64>());
        auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
        if (!bitmap.is_valid() || !bitmap.bitmap())
            return Error::from_string_literal("IPC decode: invalid filter image bitmap");
        auto color_space = TRY(decoder.decode<Gfx::ColorSpace>());
        image_frames.unchecked_append({ id, Gfx::DecodedImageFrame { *bitmap.bitmap(), move(color_space) } });
    }
    return Gfx::Filter { move(serialized_bytes), move(image_frames) };
}

}
