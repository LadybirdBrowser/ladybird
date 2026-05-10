/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::Painting {

DisplayListRecorder::DisplayListRecorder(DisplayList& command_list)
    : m_display_list(command_list)
{
}

DisplayListRecorder::~DisplayListRecorder() = default;

DisplayListRecorder::CommandCapture::CommandCapture(DisplayListRecorder& recorder)
    : m_recorder(&recorder)
{
}

DisplayListRecorder::CommandCapture::~CommandCapture()
{
    if (m_recorder)
        m_recorder->end_capture();
}

DisplayListCommandSequence DisplayListRecorder::CommandCapture::take()
{
    VERIFY(m_recorder);
    auto commands = m_recorder->m_display_list.copy_command_sequence_from(m_recorder->m_capture_start_command_offset);
    m_recorder->m_is_capturing = false;
    m_recorder = nullptr;
    return commands;
}

DisplayListRecorder::CommandCapture DisplayListRecorder::begin_command_capture()
{
    VERIFY(!m_is_capturing);
    m_is_capturing = true;
    m_capture_start_command_offset = m_display_list.command_byte_size();
    return CommandCapture(*this);
}

void DisplayListRecorder::end_capture()
{
    m_is_capturing = false;
}

template<DisplayListCommand Command>
class CommandPayloadBuilder {
public:
    explicit CommandPayloadBuilder(DisplayList const& display_list)
        : m_payload_start_offset(display_list.command_byte_size() + sizeof(DisplayListCommandHeader))
        , m_payload_size(sizeof(Command))
    {
        VERIFY(display_list.command_byte_size() % DisplayListCommandSequence::command_alignment == 0);
    }

    DisplayListDataSpan append_data(ReadonlyBytes bytes, size_t alignment)
    {
        VERIFY(alignment > 0);
        auto absolute_offset = m_payload_start_offset + m_payload_size;
        auto padded_offset = align_up_to(absolute_offset, alignment);
        auto padding = padded_offset - absolute_offset;
        auto payload_relative_offset = m_payload_size + padding;
        m_inline_payload.resize(m_inline_payload.size() + padding);
        VERIFY(payload_relative_offset <= NumericLimits<u32>::max());
        VERIFY(bytes.size() <= NumericLimits<u32>::max());
        VERIFY(payload_relative_offset + bytes.size() <= NumericLimits<u32>::max());
        if (!bytes.is_empty())
            m_inline_payload.append(bytes.data(), bytes.size());
        m_payload_size = payload_relative_offset + bytes.size();
        return { static_cast<u32>(payload_relative_offset), static_cast<u32>(bytes.size()) };
    }

    template<typename T>
    DisplayListDataSpan append_objects(Span<T> objects)
    {
        static_assert(IsTriviallyCopyable<T>);
        return append_data(ReadonlyBytes { objects.data(), objects.size() * sizeof(T) }, alignof(T));
    }

    ReadonlyBytes inline_data() const { return m_inline_payload.span(); }

private:
    size_t m_payload_start_offset { 0 };
    size_t m_payload_size { 0 };
    Vector<u8> m_inline_payload;
};

static DisplayListColorInterpolationMethod to_display_list_color_interpolation_method(
    CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod const& interpolation_method)
{
    DisplayListColorInterpolationMethod result;
    interpolation_method.visit(
        [&](CSS::RectangularColorSpace color_space) {
            result.type = DisplayListColorInterpolationMethod::Type::Rectangular;
            result.rectangular_color_space = color_space;
        },
        [&](CSS::ColorInterpolationMethodStyleValue::PolarColorInterpolationMethod const& color_space) {
            result.type = DisplayListColorInterpolationMethod::Type::Polar;
            result.polar_color_space = color_space.color_space;
            result.hue_interpolation_method = color_space.hue_interpolation_method;
        });
    return result;
}

template<DisplayListCommand Command>
static DisplayListGradientColorStops append_color_stops(
    CommandPayloadBuilder<Command>& payload_builder,
    ReadonlySpan<Color> colors,
    ReadonlySpan<float> positions,
    bool repeating = false)
{
    VERIFY(colors.size() == positions.size());
    return {
        .colors = payload_builder.append_objects(colors),
        .positions = payload_builder.append_objects(positions),
        .repeating = repeating,
    };
}

template<DisplayListCommand Command>
static DisplayListGradientColorStops append_color_stops(
    CommandPayloadBuilder<Command>& payload_builder,
    ColorStopData const& color_stops)
{
    return append_color_stops(payload_builder, color_stops.colors.span(), color_stops.positions.span(), color_stops.repeating);
}

static DisplayListGradientSpreadMethod to_display_list_gradient_spread_method(GradientPaintStyle::SpreadMethod spread_method)
{
    switch (spread_method) {
    case GradientPaintStyle::SpreadMethod::Pad:
        return DisplayListGradientSpreadMethod::Pad;
    case GradientPaintStyle::SpreadMethod::Repeat:
        return DisplayListGradientSpreadMethod::Repeat;
    case GradientPaintStyle::SpreadMethod::Reflect:
        return DisplayListGradientSpreadMethod::Reflect;
    }
    VERIFY_NOT_REACHED();
}

template<DisplayListCommand Command>
static DisplayListGradientPaintStyle to_display_list_gradient_paint_style(
    CommandPayloadBuilder<Command>& payload_builder,
    GradientPaintStyle const& paint_style)
{
    return {
        .gradient_transform = paint_style.gradient_transform(),
        .spread_method = to_display_list_gradient_spread_method(paint_style.spread_method()),
        .color_space = paint_style.color_space(),
        .color_stops = append_color_stops(payload_builder, paint_style.color_stop_colors(), paint_style.color_stop_positions()),
    };
}

template<DisplayListCommand Command>
static DisplayListPaintStyle to_display_list_paint_style(
    CommandPayloadBuilder<Command>& payload_builder,
    DisplayListResourceStorage& resource_storage,
    PaintStyle const& paint_style)
{
    DisplayListPaintStyle display_list_paint_style;
    paint_style.visit(
        [&](LinearGradientPaintStyle const& linear_gradient) {
            display_list_paint_style.type = DisplayListPaintStyleType::LinearGradient;
            display_list_paint_style.gradient = to_display_list_gradient_paint_style(payload_builder, linear_gradient);
            display_list_paint_style.linear_gradient_start_point = linear_gradient.start_point();
            display_list_paint_style.linear_gradient_end_point = linear_gradient.end_point();
        },
        [&](RadialGradientPaintStyle const& radial_gradient) {
            display_list_paint_style.type = DisplayListPaintStyleType::RadialGradient;
            display_list_paint_style.gradient = to_display_list_gradient_paint_style(payload_builder, radial_gradient);
            display_list_paint_style.radial_gradient_start_center = radial_gradient.start_center();
            display_list_paint_style.radial_gradient_start_radius = radial_gradient.start_radius();
            display_list_paint_style.radial_gradient_end_center = radial_gradient.end_center();
            display_list_paint_style.radial_gradient_end_radius = radial_gradient.end_radius();
        },
        [&](PatternPaintStyle const& pattern) {
            display_list_paint_style.type = DisplayListPaintStyleType::Pattern;
            display_list_paint_style.pattern_tile_display_list_id = resource_storage.add_display_list(pattern.tile_display_list());
            display_list_paint_style.pattern_tile_rect = pattern.tile_rect();
            display_list_paint_style.pattern_transform = pattern.pattern_transform();
        });
    return display_list_paint_style;
}

template<DisplayListCommand Command>
static DisplayListDataSpan append_path_data(CommandPayloadBuilder<Command>& payload_builder, Gfx::Path const& path)
{
    auto path_data = path.serialize_to_bytes();
    return payload_builder.append_data(path_data.span(), alignof(u32));
}

void DisplayListRecorder::replay_cached_commands(DisplayListCommandSequence const& commands)
{
    commands.for_each_command_header([&](DisplayListCommandHeader const& header, ReadonlyBytes) {
        m_save_nesting_level += display_list_command_nesting_level_change(header.type);
    });
    m_display_list.append_command_sequence(commands, m_accumulated_visual_context_index);
}

void DisplayListRecorder::paint_nested_display_list(RefPtr<DisplayList> display_list, Gfx::IntRect rect)
{
    if (!display_list)
        return;
    auto display_list_id = resource_storage().add_display_list(*display_list);
    CommandPayloadBuilder<PaintNestedDisplayList> payload_builder(m_display_list);
    auto command_bytes = payload_builder.append_data(display_list->command_bytes(), alignof(DisplayListCommandHeader));
    append_command(
        PaintNestedDisplayList {
            display_list_id,
            command_bytes,
            rect,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::add_rounded_rect_clip(CornerRadii corner_radii, Gfx::IntRect border_rect, CornerClip corner_clip)
{
    append_command(AddRoundedRectClip { corner_radii, border_rect, corner_clip });
}

void DisplayListRecorder::begin_masks(ReadonlySpan<MaskInfo> masks)
{
    for (auto const& mask : masks) {
        save();
        add_clip_rect(mask.rect);
        save_layer();
    }
}

void DisplayListRecorder::end_masks(ReadonlySpan<MaskInfo> masks)
{
    for (size_t i = masks.size(); i-- > 0;) {
        auto const& mask = masks[i];
        auto mask_kind = mask.kind == Gfx::MaskKind::Luminance ? Optional<Gfx::MaskKind>(Gfx::MaskKind::Luminance) : Optional<Gfx::MaskKind> {};
        apply_effects(1.0f, Gfx::CompositingAndBlendingOperator::DestinationIn, {}, mask_kind);
        paint_nested_display_list(mask.display_list, mask.rect);
        restore(); // DstIn layer
        restore(); // content layer
        restore(); // clip save
    }
}

void DisplayListRecorder::fill_rect(Gfx::IntRect const& rect, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    append_command(FillRect { rect, color });
}

void DisplayListRecorder::fill_rect_transparent(Gfx::IntRect const& rect)
{
    if (rect.is_empty())
        return;
    append_command(FillRect { rect, Color::Transparent });
}

void DisplayListRecorder::fill_path(FillPathParams params)
{
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_rect = params.path.bounding_box();
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    CommandPayloadBuilder<FillPath> payload_builder(m_display_list);
    auto path_span = append_path_data(payload_builder, params.path);
    auto paint_kind = PathPaintKind::Color;
    Color color;
    DisplayListPaintStyle paint_style;
    if (params.paint_style_or_color.has<PaintStyle>()) {
        paint_kind = PathPaintKind::PaintStyle;
        paint_style = to_display_list_paint_style(payload_builder, resource_storage(), params.paint_style_or_color.get<PaintStyle>());
    } else {
        color = params.paint_style_or_color.get<Gfx::Color>();
    }
    append_command(
        FillPath {
            .path_bounding_rect = path_bounding_int_rect,
            .path_data = path_span,
            .opacity = params.opacity,
            .paint_kind = paint_kind,
            .color = color,
            .paint_style = paint_style,
            .winding_rule = params.winding_rule,
            .should_anti_alias = params.should_anti_alias,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::stroke_path(StrokePathParams params)
{
    // Skia treats zero thickness as a special case and will draw a hairline, while we want to draw nothing.
    if (!params.thickness)
        return;
    if (params.paint_style_or_color.has<Gfx::Color>() && params.paint_style_or_color.get<Gfx::Color>().alpha() == 0)
        return;
    auto path_bounding_rect = params.path.bounding_box();
    // Increase path bounding box by `thickness` to account for stroke.
    path_bounding_rect.inflate(params.thickness, params.thickness);
    auto path_bounding_int_rect = enclosing_int_rect(path_bounding_rect);
    if (path_bounding_int_rect.is_empty())
        return;
    CommandPayloadBuilder<StrokePath> payload_builder(m_display_list);
    auto path_span = append_path_data(payload_builder, params.path);
    auto dash_array = payload_builder.append_objects(params.dash_array.span());
    auto paint_kind = PathPaintKind::Color;
    Color color;
    DisplayListPaintStyle paint_style;
    if (params.paint_style_or_color.has<PaintStyle>()) {
        paint_kind = PathPaintKind::PaintStyle;
        paint_style = to_display_list_paint_style(payload_builder, resource_storage(), params.paint_style_or_color.get<PaintStyle>());
    } else {
        color = params.paint_style_or_color.get<Gfx::Color>();
    }
    append_command(
        StrokePath {
            .cap_style = params.cap_style,
            .join_style = params.join_style,
            .miter_limit = params.miter_limit,
            .dash_array = dash_array,
            .dash_offset = params.dash_offset,
            .path_bounding_rect = path_bounding_int_rect,
            .path_data = path_span,
            .opacity = params.opacity,
            .paint_kind = paint_kind,
            .color = color,
            .paint_style = paint_style,
            .thickness = params.thickness,
            .should_anti_alias = params.should_anti_alias,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::draw_ellipse(Gfx::IntRect const& a_rect, Color color, int thickness)
{
    if (a_rect.is_empty() || color.alpha() == 0 || !thickness)
        return;
    append_command(DrawEllipse {
        .rect = a_rect,
        .color = color,
        .thickness = thickness,
    });
}

void DisplayListRecorder::fill_ellipse(Gfx::IntRect const& a_rect, Color color)
{
    if (a_rect.is_empty() || color.alpha() == 0)
        return;
    append_command(FillEllipse { a_rect, color });
}

void DisplayListRecorder::fill_rect_with_linear_gradient(Gfx::IntRect const& gradient_rect, LinearGradientData const& data)
{
    if (gradient_rect.is_empty())
        return;
    CommandPayloadBuilder<PaintLinearGradient> payload_builder(m_display_list);
    auto color_stops = append_color_stops(payload_builder, data.color_stops);
    auto interpolation_method = to_display_list_color_interpolation_method(data.interpolation_method);
    append_command(
        PaintLinearGradient {
            .gradient_rect = gradient_rect,
            .gradient_angle = data.gradient_angle,
            .color_stops = color_stops,
            .first_stop_position = data.first_stop_position,
            .repeat_length = data.repeat_length,
            .interpolation_method = interpolation_method,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::fill_rect_with_conic_gradient(Gfx::IntRect const& rect, ConicGradientData const& data, Gfx::IntPoint const& position)
{
    if (rect.is_empty())
        return;
    CommandPayloadBuilder<PaintConicGradient> payload_builder(m_display_list);
    auto color_stops = append_color_stops(payload_builder, data.color_stops);
    auto interpolation_method = to_display_list_color_interpolation_method(data.interpolation_method);
    append_command(
        PaintConicGradient {
            .rect = rect,
            .start_angle = data.start_angle,
            .color_stops = color_stops,
            .interpolation_method = interpolation_method,
            .position = position,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::fill_rect_with_radial_gradient(Gfx::IntRect const& rect, RadialGradientData const& data, Gfx::IntPoint center, Gfx::IntSize size)
{
    if (rect.is_empty())
        return;
    CommandPayloadBuilder<PaintRadialGradient> payload_builder(m_display_list);
    auto color_stops = append_color_stops(payload_builder, data.color_stops);
    auto interpolation_method = to_display_list_color_interpolation_method(data.interpolation_method);
    append_command(
        PaintRadialGradient {
            .rect = rect,
            .color_stops = color_stops,
            .interpolation_method = interpolation_method,
            .center = center,
            .size = size,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::draw_rect(Gfx::IntRect const& rect, Color color, bool rough)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;
    append_command(DrawRect {
        .rect = rect,
        .color = color,
        .rough = rough });
}

void DisplayListRecorder::draw_external_content(Gfx::IntRect const& dst_rect, NonnullRefPtr<ExternalContentSource> source, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    append_command(DrawExternalContent {
        .dst_rect = dst_rect,
        .source_id = resource_storage().add_external_content_source(move(source)),
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_video_frame_source(Gfx::IntRect const& dst_rect, NonnullRefPtr<VideoFrameSource> source, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    append_command(DrawVideoFrameSource {
        .dst_rect = dst_rect,
        .source_id = resource_storage().add_video_frame_source(move(source)),
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_scaled_decoded_image_frame(Gfx::IntRect const& dst_rect, Gfx::IntRect const& clip_rect, Gfx::DecodedImageFrame frame, Gfx::ScalingMode scaling_mode)
{
    if (dst_rect.is_empty())
        return;
    append_command(DrawScaledDecodedImageFrame {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .frame_id = resource_storage().add_image_frame(frame),
        .scaling_mode = scaling_mode,
    });
}

void DisplayListRecorder::draw_repeated_decoded_image_frame(Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, Gfx::DecodedImageFrame frame, Gfx::ScalingMode scaling_mode, bool repeat_x, bool repeat_y)
{
    append_command(DrawRepeatedDecodedImageFrame {
        .dst_rect = dst_rect,
        .clip_rect = clip_rect,
        .frame_id = resource_storage().add_image_frame(frame),
        .scaling_mode = scaling_mode,
        .repeat = { repeat_x, repeat_y },
    });
}

void DisplayListRecorder::draw_line(Gfx::IntPoint from, Gfx::IntPoint to, Color color, int thickness, Gfx::LineStyle style, Color alternate_color)
{
    if (color.alpha() == 0 || !thickness)
        return;
    append_command(DrawLine {
        .color = color,
        .from = from,
        .to = to,
        .thickness = thickness,
        .style = style,
        .alternate_color = alternate_color,
    });
}

void DisplayListRecorder::draw_text(Gfx::IntRect const& rect, Utf16String const& raw_text, Gfx::Font const& font, Gfx::TextAlignment alignment, Color color)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;

    auto glyph_run = Gfx::shape_text({}, 0, raw_text.utf16_view(), font, Gfx::GlyphRun::TextType::Ltr);
    float baseline_x = 0;
    if (alignment == Gfx::TextAlignment::CenterLeft) {
        baseline_x = rect.x();
    } else if (alignment == Gfx::TextAlignment::Center) {
        baseline_x = static_cast<float>(rect.x()) + (static_cast<float>(rect.width()) - glyph_run->width()) / 2.0f;
    } else if (alignment == Gfx::TextAlignment::CenterRight) {
        baseline_x = static_cast<float>(rect.right()) - glyph_run->width();
    } else {
        // Unimplemented alignment.
        TODO();
    }
    auto metrics = font.pixel_metrics();
    float baseline_y = static_cast<float>(rect.y()) + metrics.ascent + (static_cast<float>(rect.height()) - (metrics.ascent + metrics.descent)) / 2.0f;
    draw_glyph_run({ baseline_x, baseline_y }, *glyph_run, color, rect, 1.0, Orientation::Horizontal);
}

void DisplayListRecorder::draw_glyph_run(Gfx::FloatPoint baseline_start, Gfx::GlyphRun const& glyph_run, Color color, Gfx::IntRect const& rect, double scale, Orientation orientation)
{
    if (color.alpha() == 0)
        return;
    glyph_run.ensure_text_blob(scale);
    CommandPayloadBuilder<DrawGlyphRun> payload_builder(m_display_list);
    auto glyphs = payload_builder.append_objects(glyph_run.glyphs().span());
    auto glyph_bounding_rect = glyph_run.cached_blob_bounds().translated(baseline_start).to_rounded<int>();
    append_command(
        DrawGlyphRun {
            .font_id = resource_storage().add_font(glyph_run.font()),
            .glyphs = glyphs,
            .rect = rect,
            .glyph_bounding_rect = glyph_bounding_rect,
            .translation = baseline_start,
            .scale = static_cast<float>(scale),
            .color = color,
            .orientation = orientation,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::add_clip_rect(Gfx::IntRect const& rect)
{
    append_command(AddClipRect { rect });
}

void DisplayListRecorder::translate(Gfx::IntPoint delta)
{
    append_command(Translate { delta });
}

void DisplayListRecorder::save()
{
    append_command(Save {});
}

void DisplayListRecorder::save_layer()
{
    append_command(SaveLayer {});
}

void DisplayListRecorder::restore()
{
    append_command(Restore {});
}

void DisplayListRecorder::apply_backdrop_filter(Gfx::IntRect const& backdrop_region, CornerRadii const& corner_radii, Gfx::Filter const& backdrop_filter)
{
    if (backdrop_region.is_empty())
        return;
    append_command(ApplyBackdropFilter {
        .backdrop_region = backdrop_region,
        .corner_radii = corner_radii,
        .has_backdrop_filter = true,
        .backdrop_filter_id = resource_storage().add_filter(backdrop_filter),
    });
}

void DisplayListRecorder::paint_outer_box_shadow(PaintOuterBoxShadow outer_box_shadow)
{
    append_command(outer_box_shadow);
}

void DisplayListRecorder::paint_inner_box_shadow(PaintInnerBoxShadow inner_box_shadow)
{
    append_command(inner_box_shadow);
}

void DisplayListRecorder::paint_text_shadow(int blur_radius, Gfx::IntRect bounding_rect, Gfx::IntRect text_rect, Gfx::GlyphRun const& glyph_run, double glyph_run_scale, Color color, Gfx::FloatPoint draw_location)
{
    glyph_run.ensure_text_blob(glyph_run_scale);
    CommandPayloadBuilder<PaintTextShadow> payload_builder(m_display_list);
    auto glyphs = payload_builder.append_objects(glyph_run.glyphs().span());
    append_command(
        PaintTextShadow {
            .font_id = resource_storage().add_font(glyph_run.font()),
            .glyphs = glyphs,
            .shadow_bounding_rect = bounding_rect,
            .text_rect = text_rect,
            .draw_location = draw_location,
            .scale = static_cast<float>(glyph_run_scale),
            .blur_radius = blur_radius,
            .color = color,
        },
        payload_builder.inline_data());
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& rect, Color color, CornerRadii const& corner_radii)
{
    if (rect.is_empty() || color.alpha() == 0)
        return;

    if (!corner_radii.has_any_radius()) {
        fill_rect(rect, color);
        return;
    }

    append_command(FillRectWithRoundedCorners {
        .rect = rect,
        .color = color,
        .corner_radii = corner_radii,
    });
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int radius)
{
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius);
}

void DisplayListRecorder::fill_rect_with_rounded_corners(Gfx::IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    fill_rect_with_rounded_corners(a_rect, color,
        { { top_left_radius, top_left_radius },
            { top_right_radius, top_right_radius },
            { bottom_right_radius, bottom_right_radius },
            { bottom_left_radius, bottom_left_radius } });
}

void DisplayListRecorder::paint_scrollbar(ScrollFrameIndex scroll_frame_index, Gfx::IntRect gutter_rect, Gfx::IntRect thumb_rect, double scroll_size, Color thumb_color, Color track_color, bool vertical)
{
    append_command(PaintScrollBar {
        .scroll_frame_index = scroll_frame_index,
        .gutter_rect = gutter_rect,
        .thumb_rect = thumb_rect,
        .scroll_size = scroll_size,
        .thumb_color = thumb_color,
        .track_color = track_color,
        .vertical = vertical });
}

void DisplayListRecorder::apply_effects(float opacity, Gfx::CompositingAndBlendingOperator compositing_and_blending_operator, Optional<Gfx::Filter> filter, Optional<Gfx::MaskKind> mask_kind)
{
    append_command(ApplyEffects {
        .opacity = opacity,
        .compositing_and_blending_operator = compositing_and_blending_operator,
        .has_filter = filter.has_value(),
        .filter_id = filter.has_value() ? resource_storage().add_filter(filter.value()) : FilterResourceId {},
        .has_mask_kind = mask_kind.has_value(),
        .mask_kind = mask_kind.value_or({}) });
}

}
