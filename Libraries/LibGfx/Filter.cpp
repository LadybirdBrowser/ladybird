/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/NumericLimits.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/FilterImpl.h>

namespace Gfx {

static ErrorOr<Optional<ByteBuffer>> copy_optional_color_table(Optional<ReadonlyBytes> bytes)
{
    if (!bytes.has_value())
        return Optional<ByteBuffer> {};
    VERIFY(bytes->size() == 256);
    return Optional<ByteBuffer> { TRY(ByteBuffer::copy(*bytes)) };
}

Filter::Filter(Filter const& other)
    : m_impl(other.m_impl->clone())
{
}

Filter& Filter::operator=(Filter const& other)
{
    if (this != &other)
        m_impl = other.m_impl->clone();
    return *this;
}

Filter::Filter(Filter&&) = default;

Filter& Filter::operator=(Filter&&) = default;

Filter::~Filter() = default;

Filter::Filter(NonnullOwnPtr<FilterImpl>&& impl)
    : m_impl(move(impl))
{
}

FilterImpl const& Filter::impl() const
{
    return *m_impl;
}

Filter Filter::arithmetic(Optional<Filter const&> background, Optional<Filter const&> foreground, float k1, float k2, float k3, float k4)
{
    return Filter(FilterImpl::create(FilterImpl::Arithmetic {
        .background = background.copy(),
        .foreground = foreground.copy(),
        .k1 = k1,
        .k2 = k2,
        .k3 = k3,
        .k4 = k4,
    }));
}

Filter Filter::compose(Filter const& outer, Filter const& inner)
{
    return Filter(FilterImpl::create(FilterImpl::Compose {
        .outer = outer,
        .inner = inner,
    }));
}

Filter Filter::blend(Optional<Filter const&> background, Optional<Filter const&> foreground, Gfx::CompositingAndBlendingOperator mode)
{
    return Filter(FilterImpl::create(FilterImpl::Blend {
        .background = background.copy(),
        .foreground = foreground.copy(),
        .mode = mode,
    }));
}

Filter Filter::blur(float radius_x, float radius_y, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::Blur {
        .radius_x = radius_x,
        .radius_y = radius_y,
        .input = input.copy(),
    }));
}

Filter Filter::flood(Gfx::Color color, float opacity)
{
    return Filter(FilterImpl::create(FilterImpl::Flood {
        .color = color,
        .opacity = opacity,
    }));
}

Filter Filter::displacement_map(Optional<Filter const&> color, Optional<Filter const&> displacement, float scale, ChannelSelector x_channel_selector, ChannelSelector y_channel_selector)
{
    return Filter(FilterImpl::create(FilterImpl::DisplacementMap {
        .color = color.copy(),
        .displacement = displacement.copy(),
        .scale = scale,
        .x_channel_selector = x_channel_selector,
        .y_channel_selector = y_channel_selector,
    }));
}

Filter Filter::drop_shadow(float offset_x, float offset_y, float radius, Gfx::Color color, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::DropShadow {
        .offset_x = offset_x,
        .offset_y = offset_y,
        .radius = radius,
        .color = color,
        .input = input.copy(),
    }));
}

Filter Filter::color(ColorFilterType type, float amount, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::ColorFilter {
        .type = type,
        .amount = amount,
        .input = input.copy(),
    }));
}

Filter Filter::color_matrix(float matrix[20], Optional<Filter const&> input)
{
    Array<float, 20> matrix_values;
    for (size_t i = 0; i < matrix_values.size(); ++i)
        matrix_values[i] = matrix[i];
    return Filter(FilterImpl::create(FilterImpl::ColorMatrix {
        .matrix = matrix_values,
        .input = input.copy(),
    }));
}

Filter Filter::color_table(Optional<ReadonlyBytes> a, Optional<ReadonlyBytes> r, Optional<ReadonlyBytes> g, Optional<ReadonlyBytes> b, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::ColorTable {
        .a = MUST(copy_optional_color_table(a)),
        .r = MUST(copy_optional_color_table(r)),
        .g = MUST(copy_optional_color_table(g)),
        .b = MUST(copy_optional_color_table(b)),
        .input = input.copy(),
    }));
}

Filter Filter::saturate(float value, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::Saturate {
        .value = value,
        .input = input.copy(),
    }));
}

Filter Filter::hue_rotate(float angle_degrees, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::HueRotate {
        .angle_degrees = angle_degrees,
        .input = input.copy(),
    }));
}

Filter Filter::image(Gfx::DecodedImageFrame const& frame, Gfx::IntRect const& src_rect, Gfx::IntRect const& dest_rect, Gfx::ScalingMode scaling_mode)
{
    return Filter(FilterImpl::create(FilterImpl::Image {
        .frame = frame,
        .src_rect = src_rect,
        .dest_rect = dest_rect,
        .scaling_mode = scaling_mode,
    }));
}

Filter Filter::merge(Vector<Optional<Filter>> const& inputs)
{
    return Filter(FilterImpl::create(FilterImpl::Merge {
        .inputs = inputs,
    }));
}

Filter Filter::erode(float radius_x, float radius_y, Optional<Filter> const& input)
{
    return Filter(FilterImpl::create(FilterImpl::Erode {
        .radius_x = radius_x,
        .radius_y = radius_y,
        .input = input,
    }));
}

Filter Filter::dilate(float radius_x, float radius_y, Optional<Filter> const& input)
{
    return Filter(FilterImpl::create(FilterImpl::Dilate {
        .radius_x = radius_x,
        .radius_y = radius_y,
        .input = input,
    }));
}

Filter Filter::offset(float dx, float dy, Optional<Filter const&> input)
{
    return Filter(FilterImpl::create(FilterImpl::Offset {
        .dx = dx,
        .dy = dy,
        .input = input.copy(),
    }));
}

Filter Filter::turbulence(TurbulenceType turbulence_type, float base_frequency_x, float base_frequency_y, i32 num_octaves, float seed, Gfx::IntSize const& tile_stitch_size)
{
    return Filter(FilterImpl::create(FilterImpl::Turbulence {
        .turbulence_type = turbulence_type,
        .base_frequency_x = base_frequency_x,
        .base_frequency_y = base_frequency_y,
        .num_octaves = num_octaves,
        .seed = seed,
        .tile_stitch_size = tile_stitch_size,
    }));
}

namespace {

using ImageEncoder = Function<u64(Gfx::DecodedImageFrame const&)>;
using ImageDecoder = Function<Gfx::DecodedImageFrame(u64)>;

static void write_color(Stream& stream, Color color)
{
    MUST(stream.write_value<u32>(color.value()));
}

static Color read_color(Stream& stream)
{
    return Color::from_bgra(MUST(stream.read_value<u32>()));
}

static void write_int_rect(Stream& stream, Gfx::IntRect const& rect)
{
    MUST(stream.write_value<i32>(rect.x()));
    MUST(stream.write_value<i32>(rect.y()));
    MUST(stream.write_value<i32>(rect.width()));
    MUST(stream.write_value<i32>(rect.height()));
}

static Gfx::IntRect read_int_rect(Stream& stream)
{
    auto x = MUST(stream.read_value<i32>());
    auto y = MUST(stream.read_value<i32>());
    auto width = MUST(stream.read_value<i32>());
    auto height = MUST(stream.read_value<i32>());
    return Gfx::IntRect { x, y, width, height };
}

static void write_int_size(Stream& stream, Gfx::IntSize const& size)
{
    MUST(stream.write_value<i32>(size.width()));
    MUST(stream.write_value<i32>(size.height()));
}

static Gfx::IntSize read_int_size(Stream& stream)
{
    auto width = MUST(stream.read_value<i32>());
    auto height = MUST(stream.read_value<i32>());
    return Gfx::IntSize { width, height };
}

static void write_bytes(Stream& stream, ReadonlyBytes bytes)
{
    VERIFY(bytes.size() <= NumericLimits<u32>::max());
    MUST(stream.write_value<u32>(bytes.size()));
    MUST(stream.write_until_depleted(bytes));
}

static ByteBuffer read_bytes(Stream& stream)
{
    auto size = MUST(stream.read_value<u32>());
    auto buffer = MUST(ByteBuffer::create_uninitialized(size));
    MUST(stream.read_until_filled(buffer));
    return buffer;
}

static void encode_filter(Stream&, Filter const&, ImageEncoder const&);

template<typename T>
static void encode_optional_filter(Stream& stream, Optional<T> const& filter, ImageEncoder const& encode_image)
{
    MUST(stream.write_value<bool>(filter.has_value()));
    if (filter.has_value())
        encode_filter(stream, *filter, encode_image);
}

static void encode_filter(Stream& stream, Filter const& filter, ImageEncoder const& encode_image)
{
    filter.impl().operation.visit(
        [&](FilterImpl::Arithmetic const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Arithmetic));
            encode_optional_filter(stream, op.background, encode_image);
            encode_optional_filter(stream, op.foreground, encode_image);
            MUST(stream.write_value(op.k1));
            MUST(stream.write_value(op.k2));
            MUST(stream.write_value(op.k3));
            MUST(stream.write_value(op.k4));
        },
        [&](FilterImpl::Compose const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Compose));
            encode_filter(stream, op.outer, encode_image);
            encode_filter(stream, op.inner, encode_image);
        },
        [&](FilterImpl::Blend const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Blend));
            encode_optional_filter(stream, op.background, encode_image);
            encode_optional_filter(stream, op.foreground, encode_image);
            MUST(stream.write_value(op.mode));
        },
        [&](FilterImpl::Flood const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Flood));
            write_color(stream, op.color);
            MUST(stream.write_value(op.opacity));
        },
        [&](FilterImpl::DisplacementMap const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::DisplacementMap));
            encode_optional_filter(stream, op.color, encode_image);
            encode_optional_filter(stream, op.displacement, encode_image);
            MUST(stream.write_value(op.scale));
            MUST(stream.write_value(op.x_channel_selector));
            MUST(stream.write_value(op.y_channel_selector));
        },
        [&](FilterImpl::DropShadow const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::DropShadow));
            MUST(stream.write_value(op.offset_x));
            MUST(stream.write_value(op.offset_y));
            MUST(stream.write_value(op.radius));
            write_color(stream, op.color);
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Blur const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Blur));
            MUST(stream.write_value(op.radius_x));
            MUST(stream.write_value(op.radius_y));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::ColorFilter const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::ColorFilter));
            MUST(stream.write_value(op.type));
            MUST(stream.write_value(op.amount));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::ColorMatrix const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::ColorMatrix));
            for (auto value : op.matrix)
                MUST(stream.write_value(value));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::ColorTable const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::ColorTable));
            auto encode_optional_color_table = [&](Optional<ByteBuffer> const& bytes) {
                MUST(stream.write_value<bool>(bytes.has_value()));
                if (bytes.has_value())
                    write_bytes(stream, *bytes);
            };
            encode_optional_color_table(op.a);
            encode_optional_color_table(op.r);
            encode_optional_color_table(op.g);
            encode_optional_color_table(op.b);
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Saturate const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Saturate));
            MUST(stream.write_value(op.value));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::HueRotate const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::HueRotate));
            MUST(stream.write_value(op.angle_degrees));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Image const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Image));
            MUST(stream.write_value<u64>(encode_image(op.frame)));
            write_int_rect(stream, op.src_rect);
            write_int_rect(stream, op.dest_rect);
            MUST(stream.write_value(op.scaling_mode));
        },
        [&](FilterImpl::Merge const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Merge));
            VERIFY(op.inputs.size() <= NumericLimits<u32>::max());
            MUST(stream.write_value<u32>(op.inputs.size()));
            for (auto const& input : op.inputs)
                encode_optional_filter(stream, input, encode_image);
        },
        [&](FilterImpl::Offset const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Offset));
            MUST(stream.write_value(op.dx));
            MUST(stream.write_value(op.dy));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Erode const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Erode));
            MUST(stream.write_value(op.radius_x));
            MUST(stream.write_value(op.radius_y));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Dilate const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Dilate));
            MUST(stream.write_value(op.radius_x));
            MUST(stream.write_value(op.radius_y));
            encode_optional_filter(stream, op.input, encode_image);
        },
        [&](FilterImpl::Turbulence const& op) {
            MUST(stream.write_value(FilterImpl::OperationType::Turbulence));
            MUST(stream.write_value(op.turbulence_type));
            MUST(stream.write_value(op.base_frequency_x));
            MUST(stream.write_value(op.base_frequency_y));
            MUST(stream.write_value(op.num_octaves));
            MUST(stream.write_value(op.seed));
            write_int_size(stream, op.tile_stitch_size);
        });
}

static Optional<Filter> decode_optional_filter(Stream&, ImageDecoder const&);

static Filter decode_filter(Stream& stream, ImageDecoder const& decode_image)
{
    auto operation_type = MUST(stream.read_value<FilterImpl::OperationType>());
    switch (operation_type) {
    case FilterImpl::OperationType::Arithmetic: {
        auto background = decode_optional_filter(stream, decode_image);
        auto foreground = decode_optional_filter(stream, decode_image);
        auto k1 = MUST(stream.read_value<float>());
        auto k2 = MUST(stream.read_value<float>());
        auto k3 = MUST(stream.read_value<float>());
        auto k4 = MUST(stream.read_value<float>());
        return Filter::arithmetic(background, foreground, k1, k2, k3, k4);
    }
    case FilterImpl::OperationType::Compose: {
        auto outer = decode_filter(stream, decode_image);
        auto inner = decode_filter(stream, decode_image);
        return Filter::compose(outer, inner);
    }
    case FilterImpl::OperationType::Blend: {
        auto background = decode_optional_filter(stream, decode_image);
        auto foreground = decode_optional_filter(stream, decode_image);
        auto mode = MUST(stream.read_value<Gfx::CompositingAndBlendingOperator>());
        return Filter::blend(background, foreground, mode);
    }
    case FilterImpl::OperationType::Flood: {
        auto color = read_color(stream);
        auto opacity = MUST(stream.read_value<float>());
        return Filter::flood(color, opacity);
    }
    case FilterImpl::OperationType::DisplacementMap: {
        auto color = decode_optional_filter(stream, decode_image);
        auto displacement = decode_optional_filter(stream, decode_image);
        auto scale = MUST(stream.read_value<float>());
        auto x_channel_selector = MUST(stream.read_value<ChannelSelector>());
        auto y_channel_selector = MUST(stream.read_value<ChannelSelector>());
        return Filter::displacement_map(color, displacement, scale, x_channel_selector, y_channel_selector);
    }
    case FilterImpl::OperationType::DropShadow: {
        auto offset_x = MUST(stream.read_value<float>());
        auto offset_y = MUST(stream.read_value<float>());
        auto radius = MUST(stream.read_value<float>());
        auto color = read_color(stream);
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::drop_shadow(offset_x, offset_y, radius, color, input);
    }
    case FilterImpl::OperationType::Blur: {
        auto radius_x = MUST(stream.read_value<float>());
        auto radius_y = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::blur(radius_x, radius_y, input);
    }
    case FilterImpl::OperationType::ColorFilter: {
        auto type = MUST(stream.read_value<ColorFilterType>());
        auto amount = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::color(type, amount, input);
    }
    case FilterImpl::OperationType::ColorMatrix: {
        Array<float, 20> matrix_values;
        for (auto& value : matrix_values)
            value = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::color_matrix(matrix_values.data(), input);
    }
    case FilterImpl::OperationType::ColorTable: {
        auto decode_optional_color_table = [&]() -> Optional<ByteBuffer> {
            auto has_value = MUST(stream.read_value<bool>());
            if (!has_value)
                return {};
            auto bytes = read_bytes(stream);
            VERIFY(bytes.size() == 256);
            return Optional<ByteBuffer> { move(bytes) };
        };
        auto a = decode_optional_color_table();
        auto r = decode_optional_color_table();
        auto g = decode_optional_color_table();
        auto b = decode_optional_color_table();
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::color_table(a.has_value() ? Optional<ReadonlyBytes>(a->bytes()) : Optional<ReadonlyBytes> {},
            r.has_value() ? Optional<ReadonlyBytes>(r->bytes()) : Optional<ReadonlyBytes> {},
            g.has_value() ? Optional<ReadonlyBytes>(g->bytes()) : Optional<ReadonlyBytes> {},
            b.has_value() ? Optional<ReadonlyBytes>(b->bytes()) : Optional<ReadonlyBytes> {},
            input);
    }
    case FilterImpl::OperationType::Saturate: {
        auto value = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::saturate(value, input);
    }
    case FilterImpl::OperationType::HueRotate: {
        auto angle_degrees = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::hue_rotate(angle_degrees, input);
    }
    case FilterImpl::OperationType::Image: {
        auto image_id = MUST(stream.read_value<u64>());
        auto frame = decode_image(image_id);
        auto src_rect = read_int_rect(stream);
        auto dest_rect = read_int_rect(stream);
        auto scaling_mode = MUST(stream.read_value<Gfx::ScalingMode>());
        return Filter::image(frame, src_rect, dest_rect, scaling_mode);
    }
    case FilterImpl::OperationType::Merge: {
        Vector<Optional<Filter>> inputs;
        auto size = MUST(stream.read_value<u32>());
        inputs.ensure_capacity(size);
        for (size_t i = 0; i < size; ++i)
            inputs.unchecked_append(decode_optional_filter(stream, decode_image));
        return Filter::merge(inputs);
    }
    case FilterImpl::OperationType::Offset: {
        auto dx = MUST(stream.read_value<float>());
        auto dy = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::offset(dx, dy, input);
    }
    case FilterImpl::OperationType::Erode: {
        auto radius_x = MUST(stream.read_value<float>());
        auto radius_y = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::erode(radius_x, radius_y, input);
    }
    case FilterImpl::OperationType::Dilate: {
        auto radius_x = MUST(stream.read_value<float>());
        auto radius_y = MUST(stream.read_value<float>());
        auto input = decode_optional_filter(stream, decode_image);
        return Filter::dilate(radius_x, radius_y, input);
    }
    case FilterImpl::OperationType::Turbulence: {
        auto turbulence_type = MUST(stream.read_value<TurbulenceType>());
        auto base_frequency_x = MUST(stream.read_value<float>());
        auto base_frequency_y = MUST(stream.read_value<float>());
        auto num_octaves = MUST(stream.read_value<i32>());
        auto seed = MUST(stream.read_value<float>());
        auto tile_stitch_size = read_int_size(stream);
        return Filter::turbulence(turbulence_type, base_frequency_x, base_frequency_y, num_octaves, seed, tile_stitch_size);
    }
    }
    VERIFY_NOT_REACHED();
}

static Optional<Filter> decode_optional_filter(Stream& stream, ImageDecoder const& decode_image)
{
    auto has_value = MUST(stream.read_value<bool>());
    if (!has_value)
        return {};
    return decode_filter(stream, decode_image);
}

}

ByteBuffer serialize_filter(Filter const& filter, Function<u64(Gfx::DecodedImageFrame const&)> const& encode_image)
{
    AllocatingMemoryStream stream;
    encode_filter(stream, filter, encode_image);
    auto buffer = MUST(ByteBuffer::create_uninitialized(stream.used_buffer_size()));
    MUST(stream.read_until_filled(buffer));
    return buffer;
}

Filter deserialize_filter(ReadonlyBytes bytes, Function<Gfx::DecodedImageFrame(u64)> const& decode_image)
{
    FixedMemoryStream stream { bytes };
    auto filter = decode_filter(stream, decode_image);
    VERIFY(stream.is_eof());
    return filter;
}

}
