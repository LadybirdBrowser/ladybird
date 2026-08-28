/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/ParserRustFFI.h>

namespace Web::SVG {

static RustFFI::FfiSvgInput ffi_svg_input(Utf16View input)
{
    return {
        .ascii = input.has_ascii_storage() ? reinterpret_cast<u8 const*>(input.ascii_span().data()) : nullptr,
        .utf16 = input.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(input.utf16_span().data()),
        .length = input.length_in_code_units(),
    };
}

Optional<i32> parse_integer(Utf16View input)
{
    i32 value;
    if (!RustFFI::rust_parse_svg_integer(ffi_svg_input(input), &value))
        return {};
    return value;
}

float NumberPercentage::resolve_relative_to(float length) const
{
    if (!m_is_percentage)
        return m_value;
    return m_value * length;
}

Optional<NumberPercentage> parse_number_percentage(Utf16View input)
{
    float value;
    bool is_percentage;
    if (!RustFFI::rust_parse_svg_number_percentage(ffi_svg_input(input), &value, &is_percentage))
        return {};
    return NumberPercentage { value, is_percentage };
}

Vector<Gfx::FloatPoint> parse_points(Utf16View input)
{
    Vector<Gfx::FloatPoint> points;
    RustFFI::rust_parse_svg_points(ffi_svg_input(input), &points, [](void* context, RustFFI::FfiSvgPoint const* data, size_t length) {
        auto& points = *static_cast<Vector<Gfx::FloatPoint>*>(context);
        points.ensure_capacity(length);
        for (size_t i = 0; i < length; ++i)
            points.unchecked_append({ data[i].x, data[i].y });
    });
    return points;
}

Optional<Vector<Transform>> parse_transform(Utf16View input)
{
    Vector<Transform> transforms;
    if (!RustFFI::rust_parse_svg_transform(ffi_svg_input(input), &transforms, [](void* context, RustFFI::FfiSvgTransform const* data, size_t length) {
            auto& transforms = *static_cast<Vector<Transform>*>(context);
            transforms.ensure_capacity(length);
            for (size_t i = 0; i < length; ++i) {
                auto const& transform = data[i];
                switch (transform.kind) {
                case RustFFI::TRANSFORM_TRANSLATE:
                    transforms.unchecked_append({ Transform::Translate { transform.values[0], transform.values[1] } });
                    break;
                case RustFFI::TRANSFORM_SCALE:
                    transforms.unchecked_append({ Transform::Scale { transform.values[0], transform.values[1] } });
                    break;
                case RustFFI::TRANSFORM_ROTATE:
                    transforms.unchecked_append({ Transform::Rotate { transform.values[0], transform.values[1], transform.values[2] } });
                    break;
                case RustFFI::TRANSFORM_SKEW_X:
                    transforms.unchecked_append({ Transform::SkewX { transform.values[0] } });
                    break;
                case RustFFI::TRANSFORM_SKEW_Y:
                    transforms.unchecked_append({ Transform::SkewY { transform.values[0] } });
                    break;
                case RustFFI::TRANSFORM_MATRIX:
                    transforms.unchecked_append({ Transform::Matrix {
                        transform.values[0],
                        transform.values[1],
                        transform.values[2],
                        transform.values[3],
                        transform.values[4],
                        transform.values[5],
                    } });
                    break;
                default:
                    VERIFY_NOT_REACHED();
                }
            }
        })) {
        return {};
    }
    return transforms;
}

Optional<PreserveAspectRatio> parse_preserve_aspect_ratio(Utf16View input)
{
    u8 align;
    u8 meet_or_slice;
    if (!RustFFI::rust_parse_svg_preserve_aspect_ratio(ffi_svg_input(input), &align, &meet_or_slice))
        return {};
    VERIFY(align <= to_underlying(PreserveAspectRatio::Align::xMaxYMax));
    VERIFY(meet_or_slice <= to_underlying(PreserveAspectRatio::MeetOrSlice::Slice));
    return PreserveAspectRatio {
        static_cast<PreserveAspectRatio::Align>(align),
        static_cast<PreserveAspectRatio::MeetOrSlice>(meet_or_slice),
    };
}

Optional<SVGUnits> parse_units(Utf16View input)
{
    u8 value;
    if (!RustFFI::rust_parse_svg_units(ffi_svg_input(input), &value))
        return {};
    VERIFY(value <= to_underlying(SVGUnits::UserSpaceOnUse));
    return static_cast<SVGUnits>(value);
}

Vector<float> parse_table_values(Utf16View input)
{
    Vector<float> values;
    RustFFI::rust_parse_svg_table_values(ffi_svg_input(input), &values, [](void* context, float const* data, size_t length) {
        auto& values = *static_cast<Vector<float>*>(context);
        values.ensure_capacity(length);
        for (size_t i = 0; i < length; ++i)
            values.unchecked_append(data[i]);
    });
    return values;
}

Optional<SpreadMethod> parse_spread_method(Utf16View input)
{
    u8 value;
    if (!RustFFI::rust_parse_svg_spread_method(ffi_svg_input(input), &value))
        return {};
    VERIFY(value <= to_underlying(SpreadMethod::Reflect));
    return static_cast<SpreadMethod>(value);
}

Optional<ViewBox> parse_viewbox(Utf16View input)
{
    double min_x;
    double min_y;
    double width;
    double height;
    if (!RustFFI::rust_parse_svg_view_box(ffi_svg_input(input), &min_x, &min_y, &width, &height))
        return {};
    return ViewBox { min_x, min_y, width, height };
}

}
