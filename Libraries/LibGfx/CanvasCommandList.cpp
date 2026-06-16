/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Gfx {

CanvasPaintStyle to_canvas_paint_style(PaintStyle const& paint_style)
{
    if (auto const* solid_color = as_if<SolidColorPaintStyle>(paint_style))
        return solid_color->color();

    if (auto const* linear_gradient = as_if<CanvasLinearGradientPaintStyle>(paint_style)) {
        return CanvasLinearGradient {
            .start_point = linear_gradient->start_point(),
            .end_point = linear_gradient->end_point(),
            .color_stops = Vector<ColorStop> { linear_gradient->color_stops() },
            .repeat_length = linear_gradient->repeat_length(),
        };
    }

    if (auto const* radial_gradient = as_if<CanvasRadialGradientPaintStyle>(paint_style)) {
        return CanvasRadialGradient {
            .start_center = radial_gradient->start_center(),
            .start_radius = radial_gradient->start_radius(),
            .end_center = radial_gradient->end_center(),
            .end_radius = radial_gradient->end_radius(),
            .color_stops = Vector<ColorStop> { radial_gradient->color_stops() },
            .repeat_length = radial_gradient->repeat_length(),
        };
    }

    if (auto const* conic_gradient = as_if<CanvasConicGradientPaintStyle>(paint_style)) {
        return CanvasConicGradient {
            .center = conic_gradient->center(),
            .start_angle = conic_gradient->start_angle(),
            .color_stops = Vector<ColorStop> { conic_gradient->color_stops() },
            .repeat_length = conic_gradient->repeat_length(),
        };
    }

    if (auto const* pattern = as_if<CanvasPatternPaintStyle>(paint_style)) {
        return CanvasPatternStyle {
            .image = pattern->image(),
            .repetition = pattern->repetition(),
            .transform = pattern->transform(),
        };
    }

    VERIFY_NOT_REACHED();
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasLinearGradient const& gradient)
{
    TRY(encoder.encode(gradient.start_point));
    TRY(encoder.encode(gradient.end_point));
    TRY(encoder.encode(gradient.color_stops));
    TRY(encoder.encode(gradient.repeat_length));
    return {};
}

template<>
ErrorOr<Gfx::CanvasLinearGradient> decode(Decoder& decoder)
{
    return Gfx::CanvasLinearGradient {
        .start_point = TRY(decoder.decode<Gfx::FloatPoint>()),
        .end_point = TRY(decoder.decode<Gfx::FloatPoint>()),
        .color_stops = TRY(decoder.decode<Vector<Gfx::ColorStop>>()),
        .repeat_length = TRY(decoder.decode<Optional<float>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasRadialGradient const& gradient)
{
    TRY(encoder.encode(gradient.start_center));
    TRY(encoder.encode(gradient.start_radius));
    TRY(encoder.encode(gradient.end_center));
    TRY(encoder.encode(gradient.end_radius));
    TRY(encoder.encode(gradient.color_stops));
    TRY(encoder.encode(gradient.repeat_length));
    return {};
}

template<>
ErrorOr<Gfx::CanvasRadialGradient> decode(Decoder& decoder)
{
    return Gfx::CanvasRadialGradient {
        .start_center = TRY(decoder.decode<Gfx::FloatPoint>()),
        .start_radius = TRY(decoder.decode<float>()),
        .end_center = TRY(decoder.decode<Gfx::FloatPoint>()),
        .end_radius = TRY(decoder.decode<float>()),
        .color_stops = TRY(decoder.decode<Vector<Gfx::ColorStop>>()),
        .repeat_length = TRY(decoder.decode<Optional<float>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasConicGradient const& gradient)
{
    TRY(encoder.encode(gradient.center));
    TRY(encoder.encode(gradient.start_angle));
    TRY(encoder.encode(gradient.color_stops));
    TRY(encoder.encode(gradient.repeat_length));
    return {};
}

template<>
ErrorOr<Gfx::CanvasConicGradient> decode(Decoder& decoder)
{
    return Gfx::CanvasConicGradient {
        .center = TRY(decoder.decode<Gfx::FloatPoint>()),
        .start_angle = TRY(decoder.decode<float>()),
        .color_stops = TRY(decoder.decode<Vector<Gfx::ColorStop>>()),
        .repeat_length = TRY(decoder.decode<Optional<float>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasPatternStyle const& pattern)
{
    TRY(encoder.encode(pattern.image));
    TRY(encoder.encode(pattern.repetition));
    TRY(encoder.encode(pattern.transform));
    return {};
}

template<>
ErrorOr<Gfx::CanvasPatternStyle> decode(Decoder& decoder)
{
    return Gfx::CanvasPatternStyle {
        .image = TRY(decoder.decode<Optional<Gfx::DecodedImageFrame>>()),
        .repetition = TRY(decoder.decode<Gfx::CanvasPatternPaintStyle::Repetition>()),
        .transform = TRY(decoder.decode<Optional<Gfx::AffineTransform>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::ClearRect const& command)
{
    TRY(encoder.encode(command.rect));
    TRY(encoder.encode(command.color));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::ClearRect> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::ClearRect {
        .rect = TRY(decoder.decode<Gfx::FloatRect>()),
        .color = TRY(decoder.decode<Gfx::Color>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::FillRect const& command)
{
    TRY(encoder.encode(command.rect));
    TRY(encoder.encode(command.color));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::FillRect> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::FillRect {
        .rect = TRY(decoder.decode<Gfx::FloatRect>()),
        .color = TRY(decoder.decode<Gfx::Color>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::DrawBitmap const& command)
{
    TRY(encoder.encode(command.frame));
    TRY(encoder.encode(command.dst_rect));
    TRY(encoder.encode(command.src_rect));
    TRY(encoder.encode(command.scaling_mode));
    TRY(encoder.encode(command.filter));
    TRY(encoder.encode(command.global_alpha));
    TRY(encoder.encode(command.compositing_and_blending_operator));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::DrawBitmap> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::DrawBitmap {
        .frame = TRY(decoder.decode<Gfx::DecodedImageFrame>()),
        .dst_rect = TRY(decoder.decode<Gfx::FloatRect>()),
        .src_rect = TRY(decoder.decode<Gfx::IntRect>()),
        .scaling_mode = TRY(decoder.decode<Gfx::ScalingMode>()),
        .filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
        .global_alpha = TRY(decoder.decode<float>()),
        .compositing_and_blending_operator = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::DrawCanvas const& command)
{
    TRY(encoder.encode(command.source_canvas_id));
    TRY(encoder.encode(command.dst_rect));
    TRY(encoder.encode(command.src_rect));
    TRY(encoder.encode(command.scaling_mode));
    TRY(encoder.encode(command.filter));
    TRY(encoder.encode(command.global_alpha));
    TRY(encoder.encode(command.compositing_and_blending_operator));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::DrawCanvas> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::DrawCanvas {
        .source_canvas_id = TRY(decoder.decode<u64>()),
        .dst_rect = TRY(decoder.decode<Gfx::FloatRect>()),
        .src_rect = TRY(decoder.decode<Gfx::IntRect>()),
        .scaling_mode = TRY(decoder.decode<Gfx::ScalingMode>()),
        .filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
        .global_alpha = TRY(decoder.decode<float>()),
        .compositing_and_blending_operator = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::FillPath const& command)
{
    TRY(encoder.encode(command.path));
    TRY(encoder.encode(command.style));
    TRY(encoder.encode(command.winding_rule));
    TRY(encoder.encode(command.blur_radius));
    TRY(encoder.encode(command.filter));
    TRY(encoder.encode(command.global_alpha));
    TRY(encoder.encode(command.compositing_and_blending_operator));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::FillPath> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::FillPath {
        .path = TRY(decoder.decode<Gfx::Path>()),
        .style = TRY(decoder.decode<Gfx::CanvasPaintStyle>()),
        .winding_rule = TRY(decoder.decode<Gfx::WindingRule>()),
        .blur_radius = TRY(decoder.decode<float>()),
        .filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
        .global_alpha = TRY(decoder.decode<float>()),
        .compositing_and_blending_operator = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::StrokePath const& command)
{
    TRY(encoder.encode(command.path));
    TRY(encoder.encode(command.style));
    TRY(encoder.encode(command.thickness));
    TRY(encoder.encode(command.cap_style));
    TRY(encoder.encode(command.join_style));
    TRY(encoder.encode(command.miter_limit));
    TRY(encoder.encode(command.dash_array));
    TRY(encoder.encode(command.dash_offset));
    TRY(encoder.encode(command.blur_radius));
    TRY(encoder.encode(command.filter));
    TRY(encoder.encode(command.global_alpha));
    TRY(encoder.encode(command.compositing_and_blending_operator));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::StrokePath> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::StrokePath {
        .path = TRY(decoder.decode<Gfx::Path>()),
        .style = TRY(decoder.decode<Gfx::CanvasPaintStyle>()),
        .thickness = TRY(decoder.decode<float>()),
        .cap_style = TRY(decoder.decode<Gfx::Path::CapStyle>()),
        .join_style = TRY(decoder.decode<Gfx::Path::JoinStyle>()),
        .miter_limit = TRY(decoder.decode<float>()),
        .dash_array = TRY(decoder.decode<Vector<float>>()),
        .dash_offset = TRY(decoder.decode<float>()),
        .blur_radius = TRY(decoder.decode<float>()),
        .filter = TRY(decoder.decode<Optional<Gfx::Filter>>()),
        .global_alpha = TRY(decoder.decode<float>()),
        .compositing_and_blending_operator = TRY(decoder.decode<Gfx::CompositingAndBlendingOperator>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::SetTransform const& command)
{
    TRY(encoder.encode(command.transform));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::SetTransform> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::SetTransform {
        .transform = TRY(decoder.decode<Gfx::AffineTransform>()),
    };
}

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Save const&)
{
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::Save> decode(Decoder&)
{
    return Gfx::CanvasCommands::Save {};
}

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Restore const&)
{
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::Restore> decode(Decoder&)
{
    return Gfx::CanvasCommands::Restore {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommands::ClipPath const& command)
{
    TRY(encoder.encode(command.path));
    TRY(encoder.encode(command.winding_rule));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::ClipPath> decode(Decoder& decoder)
{
    return Gfx::CanvasCommands::ClipPath {
        .path = TRY(decoder.decode<Gfx::Path>()),
        .winding_rule = TRY(decoder.decode<Gfx::WindingRule>()),
    };
}

template<>
ErrorOr<void> encode(Encoder&, Gfx::CanvasCommands::Reset const&)
{
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommands::Reset> decode(Decoder&)
{
    return Gfx::CanvasCommands::Reset {};
}

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::CanvasCommandList const& command_list)
{
    TRY(encoder.encode(command_list.commands()));
    return {};
}

template<>
ErrorOr<Gfx::CanvasCommandList> decode(Decoder& decoder)
{
    return Gfx::CanvasCommandList { TRY(decoder.decode<Vector<Gfx::CanvasCommand>>()) };
}

}
