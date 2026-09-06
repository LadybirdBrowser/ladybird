/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    FilterWriter writer { FFI::FilterOperationType::Compose };
    writer.filter(outer);
    writer.filter(inner);
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

Filter Filter::hue_rotate(float angle_degrees, Optional<Filter const&> input)
{
    FilterWriter writer { FFI::FilterOperationType::HueRotate };
    writer.value(angle_degrees);
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
