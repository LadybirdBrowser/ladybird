/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NumericLimits.h>
#include <AK/TypeCasts.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/FilterImpl.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/SkiaUtils.h>
#include <LibPaintServer/Compositor/CanvasPainter.h>
#include <core/SkData.h>
#include <core/SkImage.h>
#include <core/SkImageFilter.h>
#include <core/SkPath.h>
#include <core/SkSerialProcs.h>

namespace PaintServer {

static Optional<ByteBuffer> command_buffer(ReadonlySpan<ReadonlyBytes const> fragments, size_t command_size)
{
    auto buffer_or_error = ByteBuffer::create_uninitialized(command_size);
    if (buffer_or_error.is_error())
        return {};
    auto buffer = buffer_or_error.release_value();
    size_t offset = 0;
    for (ReadonlyBytes fragment : fragments) {
        if (fragment.is_empty())
            continue;
        if (fragment.size() > buffer.size() - offset)
            return {};
        fragment.copy_to(buffer.bytes().slice(offset, fragment.size()));
        offset += fragment.size();
    }
    if (offset != command_size)
        return {};
    return buffer;
}

struct FilterImageSerializationContext {
    CanvasPainter::ImageResourceResolver const& resolve_image_resource;
    HashMap<u32, Gfx::DecodedImageFrame const*> frames_by_skia_image_unique_id;
};

static sk_sp<SkData const> serialize_filter_image(SkImage* image, void* context)
{
    if (!image || !context)
        return nullptr;

    auto& serialization_context = *static_cast<FilterImageSerializationContext*>(context);
    auto it = serialization_context.frames_by_skia_image_unique_id.find(image->uniqueID());
    if (it == serialization_context.frames_by_skia_image_unique_id.end())
        return nullptr;

    auto image_resource = serialization_context.resolve_image_resource(*it->value);
    if (!image_resource.has_value())
        return nullptr;

    SerializedFilterImageReference serialized_reference {
        .image_resource_id = image_resource->image_resource_id,
        .image_id = image_resource->image_id,
    };
    return SkData::MakeWithCopy(&serialized_reference, sizeof(serialized_reference));
}

static Optional<ByteBuffer> path_command_buffer(auto& command, Gfx::Path const& path, ReadonlyBytes leading_bytes = {}, ReadonlyBytes middle_bytes = {}, ReadonlyBytes trailing_bytes = {})
{
    SkPath sk_path = Gfx::to_skia_path(path);
    sk_sp<SkData> path_data = sk_path.serialize();
    if (!path_data || path_data->size() > NumericLimits<u32>::max())
        return {};

    ReadonlyBytes path_bytes { path_data->data(), path_data->size() };
    command.path_byte_count = static_cast<u32>(path_bytes.size());
    size_t command_size = sizeof(command) + leading_bytes.size() + path_bytes.size() + middle_bytes.size() + trailing_bytes.size();
    if (command_size > NumericLimits<u32>::max())
        return {};

    command.command_size = static_cast<u32>(command_size);
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { &command, sizeof(command) },
        leading_bytes,
        path_bytes,
        middle_bytes,
        trailing_bytes,
    };
    return command_buffer(fragments, command.command_size);
}

Optional<ByteBuffer> CanvasPainter::serialize_filter_bytes(Optional<Gfx::Filter> const& filter) const
{
    if (!filter.has_value())
        return ByteBuffer {};

    sk_sp<SkImageFilter> sk_filter = Gfx::to_skia_image_filter(filter.value());
    if (!sk_filter)
        return ByteBuffer {};

    FilterImageSerializationContext serialization_context {
        .resolve_image_resource = m_resolve_image_resource,
        .frames_by_skia_image_unique_id = {},
    };
    auto const& image_references = filter->impl().image_references;
    if (!image_references.is_empty() && !m_resolve_image_resource)
        return {};
    if (serialization_context.frames_by_skia_image_unique_id.try_ensure_capacity(image_references.size()).is_error())
        return {};
    for (auto const& image_reference : image_references)
        serialization_context.frames_by_skia_image_unique_id.set(image_reference.skia_image_unique_id, image_reference.frame.ptr());

    SkSerialProcs serial_procs;
    serial_procs.fImageCtx = &serialization_context;
    serial_procs.fImageProc = serialize_filter_image;

    sk_sp<SkData> data = sk_filter->serialize(&serial_procs);
    if (!data)
        return {};

    constexpr size_t max_filter_bytes = static_cast<size_t>(1024) * static_cast<size_t>(1024);
    if (data->size() > max_filter_bytes || data->size() > NumericLimits<u32>::max())
        return {};

    auto buffer = ByteBuffer::copy(data->data(), data->size());
    if (buffer.is_error())
        return {};
    return buffer.release_value();
}

static Optional<ByteBuffer> paint_payload_buffer(auto const& header, ReadonlySpan<Gfx::ColorStop const> color_stops)
{
    if (color_stops.size() > NumericLimits<u32>::max() / sizeof(WireColorStop))
        return {};

    size_t payload_size = sizeof(header) + (color_stops.size() * sizeof(WireColorStop));
    auto payload_or_error = ByteBuffer::create_uninitialized(payload_size);
    if (payload_or_error.is_error())
        return {};

    auto payload = payload_or_error.release_value();
    size_t offset = 0;
    ReadonlyBytes { &header, sizeof(header) }.copy_to(payload.bytes().slice(offset, sizeof(header)));
    offset += sizeof(header);
    for (auto const& stop : color_stops) {
        WireColorStop wire_stop { .color = stop.color, .position = stop.position };
        ReadonlyBytes { &wire_stop, sizeof(wire_stop) }.copy_to(payload.bytes().slice(offset, sizeof(wire_stop)));
        offset += sizeof(wire_stop);
    }
    return payload;
}

CanvasPainter::CanvasPainter(ImageResourceResolver resolve_image_resource, Gfx::PaintingSurface* shadow_surface)
    : m_resolve_image_resource(move(resolve_image_resource))
{
    if (shadow_surface)
        m_shadow_painter = make<Gfx::PainterSkia>(*shadow_surface);
}
CanvasPainter::~CanvasPainter() = default;

DrawList CanvasPainter::take_draw_list()
{
    DrawList draw_list = move(m_draw_list);
    m_draw_list = {};
    m_has_error = false;
    return draw_list;
}

void CanvasPainter::clear_draw_list()
{
    m_draw_list.clear();
    m_has_error = false;
}

void CanvasPainter::append_command(ReadonlyBytes bytes)
{
    auto result = m_draw_list.try_append_command(bytes);
    if (result.is_error()) {
        dbgln("CanvasPainter: failed to append command: {}", result.error());
        m_has_error = true;
    }
}

bool CanvasPainter::encode_paint_style(Gfx::PaintStyle const& paint_style, u8& paint_style_type, Gfx::Color& color, ByteBuffer& paint_style_payload) const
{
    if (auto const* solid_color = as_if<Gfx::SolidColorPaintStyle>(&paint_style)) {
        paint_style_type = to_underlying(PaintStyleType::SolidColor);
        color = solid_color->color();
        paint_style_payload.clear();
        return true;
    }
    if (auto const* linear_gradient = as_if<Gfx::CanvasLinearGradientPaintStyle>(&paint_style)) {
        CanvasLinearGradientPayload header {
            .start_point = linear_gradient->start_point(),
            .end_point = linear_gradient->end_point(),
            .stop_count = static_cast<u32>(linear_gradient->color_stops().size()),
        };
        auto payload = paint_payload_buffer(header, linear_gradient->color_stops());
        if (!payload.has_value())
            return false;

        paint_style_type = to_underlying(PaintStyleType::CanvasLinearGradient);
        color = {};
        paint_style_payload = payload.release_value();
        return true;
    }
    if (auto const* radial_gradient = as_if<Gfx::CanvasRadialGradientPaintStyle>(&paint_style)) {
        CanvasRadialGradientPayload header {
            .start_center = radial_gradient->start_center(),
            .start_radius = radial_gradient->start_radius(),
            .end_center = radial_gradient->end_center(),
            .end_radius = radial_gradient->end_radius(),
            .stop_count = static_cast<u32>(radial_gradient->color_stops().size()),
        };
        auto payload = paint_payload_buffer(header, radial_gradient->color_stops());
        if (!payload.has_value())
            return false;

        paint_style_type = to_underlying(PaintStyleType::CanvasRadialGradient);
        color = {};
        paint_style_payload = payload.release_value();
        return true;
    }
    if (auto const* conic_gradient = as_if<Gfx::CanvasConicGradientPaintStyle>(&paint_style)) {
        CanvasConicGradientPayload header {
            .center = conic_gradient->center(),
            .start_angle = conic_gradient->start_angle(),
            .stop_count = static_cast<u32>(conic_gradient->color_stops().size()),
        };
        auto payload = paint_payload_buffer(header, conic_gradient->color_stops());
        if (!payload.has_value())
            return false;

        paint_style_type = to_underlying(PaintStyleType::CanvasConicGradient);
        color = {};
        paint_style_payload = payload.release_value();
        return true;
    }
    if (auto const* pattern = as_if<Gfx::CanvasPatternPaintStyle>(&paint_style)) {
        auto image = pattern->image();
        if (!image || !m_resolve_image_resource)
            return false;
        auto image_resource = m_resolve_image_resource(*image);
        if (!image_resource.has_value())
            return false;

        auto repetition = pattern->repetition();
        bool repeat_x = repetition == Gfx::CanvasPatternPaintStyle::Repetition::Repeat || repetition == Gfx::CanvasPatternPaintStyle::Repetition::RepeatX;
        bool repeat_y = repetition == Gfx::CanvasPatternPaintStyle::Repetition::Repeat || repetition == Gfx::CanvasPatternPaintStyle::Repetition::RepeatY;
        CanvasPatternPayload payload {
            .image_resource_id = image_resource->image_resource_id,
            .image_id = image_resource->image_id,
            .transform = pattern->transform().value_or(Gfx::AffineTransform {}),
            .repeat_x = static_cast<u8>(repeat_x ? 1 : 0),
            .repeat_y = static_cast<u8>(repeat_y ? 1 : 0),
            .has_transform = static_cast<u8>(pattern->transform().has_value() ? 1 : 0),
        };
        auto buffer_or_error = ByteBuffer::copy(ReadonlyBytes { &payload, sizeof(payload) });
        if (buffer_or_error.is_error())
            return false;

        paint_style_type = to_underlying(PaintStyleType::CanvasPattern);
        color = {};
        paint_style_payload = buffer_or_error.release_value();
        return true;
    }

    return false;
}

void CanvasPainter::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    if (m_shadow_painter)
        m_shadow_painter->clear_rect(rect, color);
    ClearRectCommand command { .color = color, .rect = rect };
    append_command(ReadonlyBytes { &command, sizeof(command) });
}

void CanvasPainter::fill_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    if (m_shadow_painter)
        m_shadow_painter->fill_rect(rect, color);
    FillRectCommand command { .color = color, .rect = rect };
    append_command(ReadonlyBytes { &command, sizeof(command) });
}

void CanvasPainter::draw_bitmap(Gfx::FloatRect const& dst_rect, Gfx::DecodedImageFrame const& source, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Optional<Gfx::Filter> filters, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    if (m_shadow_painter)
        m_shadow_painter->draw_bitmap(dst_rect, source, src_rect, scaling_mode, filters, global_alpha, compositing_and_blending_operator);

    if (dst_rect.is_empty() || src_rect.is_empty())
        return;
    if (!m_resolve_image_resource) {
        m_has_error = true;
        return;
    }
    auto image_resource = m_resolve_image_resource(source);
    if (!image_resource.has_value()) {
        m_has_error = true;
        return;
    }
    auto filter_bytes = serialize_filter_bytes(filters);
    if (!filter_bytes.has_value()) {
        m_has_error = true;
        return;
    }
    DrawScaledImageCommand command {
        .image_resource_id = image_resource->image_resource_id,
        .image_id = image_resource->image_id,
        .scaling_mode = static_cast<u32>(to_underlying(scaling_mode)),
        .opacity = global_alpha,
        .compositing_and_blending_operator = static_cast<u8>(to_underlying(compositing_and_blending_operator)),
        .filter_byte_count = static_cast<u32>(filter_bytes->size()),
        .src_rect = src_rect.to_type<f32>(),
        .dst_rect = dst_rect,
        .clip_rect = dst_rect,
    };
    size_t command_size = sizeof(command) + filter_bytes->size();
    if (command_size > NumericLimits<u32>::max()) {
        m_has_error = true;
        return;
    }
    command.command_size = static_cast<u32>(command_size);
    ReadonlyBytes const fragments[] {
        ReadonlyBytes { &command, sizeof(command) },
        filter_bytes->bytes(),
    };
    auto buffer = command_buffer(fragments, command_size);
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness)
{
    if (m_shadow_painter)
        m_shadow_painter->stroke_path(path, color, thickness);
    append_stroke_path_command(path, color, {}, 1.0f, thickness, Gfx::CompositingAndBlendingOperator::SourceOver, Gfx::Path::CapStyle::Butt, Gfx::Path::JoinStyle::Miter, 4.0f, {}, 0.0f);
}

void CanvasPainter::stroke_path(Gfx::Path const& path, Gfx::Color color, float thickness, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    if (m_shadow_painter)
        m_shadow_painter->stroke_path(path, color, thickness, blur_radius, compositing_and_blending_operator, cap_style, join_style, miter_limit, dash_array, dash_offset);

    append_stroke_path_command(path, color, {}, 1.0f, thickness, compositing_and_blending_operator, cap_style, join_style, miter_limit, dash_array, dash_offset, blur_radius);
}

void CanvasPainter::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    if (m_shadow_painter)
        m_shadow_painter->stroke_path(path, paint_style, filter, thickness, global_alpha, compositing_and_blending_operator);
    append_stroke_path_command(path, paint_style, filter, global_alpha, thickness, compositing_and_blending_operator, Gfx::Path::CapStyle::Butt, Gfx::Path::JoinStyle::Miter, 4.0f, {}, 0.0f);
}

void CanvasPainter::stroke_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float thickness, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle const& cap_style, Gfx::Path::JoinStyle const& join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    if (m_shadow_painter)
        m_shadow_painter->stroke_path(path, paint_style, filter, thickness, global_alpha, compositing_and_blending_operator, cap_style, join_style, miter_limit, dash_array, dash_offset);
    append_stroke_path_command(path, paint_style, filter, global_alpha, thickness, compositing_and_blending_operator, cap_style, join_style, miter_limit, dash_array, dash_offset);
}

void CanvasPainter::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule)
{
    if (m_shadow_painter)
        m_shadow_painter->fill_path(path, color, winding_rule);
    append_path_command(path, color, {}, 1.0f, Gfx::CompositingAndBlendingOperator::SourceOver, winding_rule);
}

void CanvasPainter::fill_path(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule, float blur_radius, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    if (m_shadow_painter)
        m_shadow_painter->fill_path(path, color, winding_rule, blur_radius, compositing_and_blending_operator);

    append_path_command(path, color, {}, 1.0f, compositing_and_blending_operator, winding_rule, blur_radius);
}

void CanvasPainter::fill_path(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> filter, float global_alpha, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule)
{
    if (m_shadow_painter)
        m_shadow_painter->fill_path(path, paint_style, filter, global_alpha, compositing_and_blending_operator, winding_rule);
    append_path_command(path, paint_style, filter, global_alpha, compositing_and_blending_operator, winding_rule);
}

void CanvasPainter::append_path_command(Gfx::Path const& path, Gfx::Color color, Optional<Gfx::Filter> const& filter, float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule, float blur_radius)
{
    auto filter_bytes = serialize_filter_bytes(filter);
    if (!filter_bytes.has_value()) {
        m_has_error = true;
        return;
    }
    FillPathCommand command;
    command.color = color;
    command.path_bounding_rect = path.bounding_box();
    command.opacity = opacity;
    command.blur_radius = blur_radius;
    command.winding_rule = static_cast<u8>(to_underlying(winding_rule));
    command.compositing_and_blending_operator = static_cast<u8>(to_underlying(compositing_and_blending_operator));
    command.filter_byte_count = static_cast<u32>(filter_bytes->size());

    auto buffer = path_command_buffer(command, path, {}, filter_bytes->bytes());
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::append_path_command(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> const& filter, float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::WindingRule winding_rule)
{
    auto filter_bytes = serialize_filter_bytes(filter);
    if (!filter_bytes.has_value()) {
        m_has_error = true;
        return;
    }
    u8 paint_style_type = to_underlying(PaintStyleType::SolidColor);
    Gfx::Color color;
    ByteBuffer paint_payload;
    if (!encode_paint_style(paint_style, paint_style_type, color, paint_payload)) {
        m_has_error = true;
        return;
    }
    FillPathCommand command;
    command.color = color;
    command.path_bounding_rect = path.bounding_box();
    command.opacity = opacity;
    command.winding_rule = static_cast<u8>(to_underlying(winding_rule));
    command.compositing_and_blending_operator = static_cast<u8>(to_underlying(compositing_and_blending_operator));
    command.filter_byte_count = static_cast<u32>(filter_bytes->size());
    command.paint_style_type = paint_style_type;
    command.paint_style_byte_count = static_cast<u32>(paint_payload.size());

    auto buffer = path_command_buffer(command, path, {}, filter_bytes->bytes(), paint_payload.bytes());
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::append_stroke_path_command(Gfx::Path const& path, Gfx::Color color, Optional<Gfx::Filter> const& filter, float opacity, float thickness, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset, float blur_radius)
{
    if (thickness <= 0.0f)
        return;
    auto filter_bytes = serialize_filter_bytes(filter);
    if (!filter_bytes.has_value()) {
        m_has_error = true;
        return;
    }
    if (dash_array.size() > NumericLimits<u32>::max() / sizeof(float)) {
        m_has_error = true;
        return;
    }
    StrokePathCommand command;
    command.color = color;
    command.path_bounding_rect = path.bounding_box();
    command.opacity = opacity;
    command.thickness = thickness;
    command.miter_limit = miter_limit;
    command.dash_offset = dash_offset;
    command.blur_radius = blur_radius;
    command.dash_count = static_cast<u32>(dash_array.size());
    command.cap_style = static_cast<u8>(to_underlying(cap_style));
    command.join_style = static_cast<u8>(to_underlying(join_style));
    command.compositing_and_blending_operator = static_cast<u8>(to_underlying(compositing_and_blending_operator));
    command.filter_byte_count = static_cast<u32>(filter_bytes->size());

    ReadonlyBytes dash_bytes { dash_array.data(), dash_array.size() * sizeof(float) };
    auto buffer = path_command_buffer(command, path, dash_bytes, filter_bytes->bytes());
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::append_stroke_path_command(Gfx::Path const& path, Gfx::PaintStyle const& paint_style, Optional<Gfx::Filter> const& filter, float opacity, float thickness, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Gfx::Path::CapStyle cap_style, Gfx::Path::JoinStyle join_style, float miter_limit, Vector<float> const& dash_array, float dash_offset)
{
    if (thickness <= 0.0f)
        return;
    auto filter_bytes = serialize_filter_bytes(filter);
    if (!filter_bytes.has_value()) {
        m_has_error = true;
        return;
    }
    if (dash_array.size() > NumericLimits<u32>::max() / sizeof(float)) {
        m_has_error = true;
        return;
    }
    u8 paint_style_type = to_underlying(PaintStyleType::SolidColor);
    Gfx::Color color;
    ByteBuffer paint_payload;
    if (!encode_paint_style(paint_style, paint_style_type, color, paint_payload)) {
        m_has_error = true;
        return;
    }
    StrokePathCommand command;
    command.color = color;
    command.path_bounding_rect = path.bounding_box();
    command.opacity = opacity;
    command.thickness = thickness;
    command.miter_limit = miter_limit;
    command.dash_offset = dash_offset;
    command.dash_count = static_cast<u32>(dash_array.size());
    command.cap_style = static_cast<u8>(to_underlying(cap_style));
    command.join_style = static_cast<u8>(to_underlying(join_style));
    command.compositing_and_blending_operator = static_cast<u8>(to_underlying(compositing_and_blending_operator));
    command.filter_byte_count = static_cast<u32>(filter_bytes->size());
    command.paint_style_type = paint_style_type;
    command.paint_style_byte_count = static_cast<u32>(paint_payload.size());

    ReadonlyBytes dash_bytes { dash_array.data(), dash_array.size() * sizeof(float) };
    auto buffer = path_command_buffer(command, path, dash_bytes, filter_bytes->bytes(), paint_payload.bytes());
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::set_transform(Gfx::AffineTransform const& transform)
{
    if (m_shadow_painter)
        m_shadow_painter->set_transform(transform);
    SetTransformCommand command { .transform = transform };
    append_command(ReadonlyBytes { &command, sizeof(command) });
}

void CanvasPainter::save()
{
    if (m_shadow_painter)
        m_shadow_painter->save();
    SaveCommand command;
    append_command(ReadonlyBytes { &command, sizeof(command) });
    ++m_save_depth;
}

void CanvasPainter::restore()
{
    if (m_save_depth == 0)
        return;
    if (m_shadow_painter)
        m_shadow_painter->restore();
    RestoreCommand command;
    append_command(ReadonlyBytes { &command, sizeof(command) });
    --m_save_depth;
}

void CanvasPainter::clip(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    if (m_shadow_painter)
        m_shadow_painter->clip(path, winding_rule);

    AddClipPathCommand command;
    command.bounding_rect = path.bounding_box();
    command.fill_rule = static_cast<u8>(to_underlying(winding_rule));

    auto buffer = path_command_buffer(command, path);
    if (!buffer.has_value()) {
        m_has_error = true;
        return;
    }
    append_command(buffer->bytes());
}

void CanvasPainter::reset()
{
    if (m_shadow_painter)
        m_shadow_painter->reset();
    ResetCanvasStateCommand command;
    append_command(ReadonlyBytes { &command, sizeof(command) });
    m_save_depth = 0;
}

}
