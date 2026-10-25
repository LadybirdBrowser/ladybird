/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/FontSkia.h>
#include <Compositor/PainterSkia.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/DisplayList/Canvas2DTape.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Path.h>
#include <core/SkCanvas.h>
#include <core/SkFont.h>
#include <core/SkPaint.h>

namespace Compositor {

using namespace Compositing;

Canvas2DCommandPlayer::Canvas2DCommandPlayer(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, Gfx::IntSize size, Gfx::BitmapFormat format, Gfx::AlphaType alpha_type, Gfx::ColorSpace const& color_space, Resolvers resolvers)
    : m_color_space(color_space)
    , m_surface(Gfx::PaintingSurface::create_with_size(size, format, alpha_type, move(skia_backend_context), m_color_space))
    , m_painter(make<Gfx::PainterSkia>(*m_surface))
    , m_resolvers(move(resolvers))
{
}

Canvas2DCommandPlayer::~Canvas2DCommandPlayer() = default;

NonnullRefPtr<Gfx::PaintingSurface> Canvas2DCommandPlayer::surface() const
{
    return m_surface;
}

void Canvas2DCommandPlayer::clear(Gfx::Color color)
{
    m_painter->clear_rect({ {}, m_surface->size().to_type<float>() }, color);
}

void Canvas2DCommandPlayer::play(ReadonlyBytes tape)
{
    for_each_canvas_2d_command(tape, [&](DisplayListCommandType command_type, ReadonlyBytes payload) {
        m_payload = payload;
        switch (command_type) {
#define PLAY_CANVAS_2D_COMMAND(command_type)                                        \
    case DisplayListCommandType::command_type:                                      \
        play_command(read_display_list_object<Compositing::command_type>(payload)); \
        break;
            ENUMERATE_CANVAS_2D_COMMANDS(PLAY_CANVAS_2D_COMMAND)
#undef PLAY_CANVAS_2D_COMMAND
        default:
            // NB: Validation lets only the commands of the canvas 2D stream onto the tape of a canvas.
            VERIFY_NOT_REACHED();
        }
    });
    m_payload = {};
}

ReadonlyBytes Canvas2DCommandPlayer::inline_data(DisplayListDataSpan span) const
{
    return m_payload.slice(span.offset, span.size);
}

// NB: The tape is not aligned for the values of an array span, so they are copied out.
template<typename T>
static Vector<T> inline_objects(ReadonlyBytes bytes)
{
    Vector<T> objects;
    objects.resize(bytes.size() / sizeof(T));
    if (!objects.is_empty())
        __builtin_memcpy(objects.data(), bytes.data(), objects.size() * sizeof(T));
    return objects;
}

Gfx::Path Canvas2DCommandPlayer::path_from(DisplayListDataSpan path_data) const
{
    return Gfx::Path::from_serialized_bytes(inline_data(path_data));
}

sk_sp<SkImageFilter> Canvas2DCommandPlayer::image_filter_from(Canvas2DCompositing const& compositing) const
{
    if (!compositing.has_filter)
        return nullptr;
    return Gfx::to_skia_image_filter(inline_data(compositing.filter), [](u64) -> Gfx::DecodedImageFrame const& {
        // A canvas filter comes from CSS filter functions, which draw no images.
        VERIFY_NOT_REACHED();
    });
}

static Optional<Gfx::CanvasShadow> shadow_from(Canvas2DCompositing const& compositing)
{
    if (!compositing.has_shadow)
        return {};
    return Gfx::CanvasShadow {
        .color = compositing.shadow.color,
        .offset_x = compositing.shadow.offset_x,
        .offset_y = compositing.shadow.offset_y,
        .blur = compositing.shadow.blur,
    };
}

RefPtr<Gfx::PaintStyle> Canvas2DCommandPlayer::paint_style_from(Canvas2DPaint const& paint) const
{
    auto with_color_stops = [&](auto paint_style) -> NonnullRefPtr<Gfx::PaintStyle> {
        auto colors = inline_objects<Gfx::Color>(inline_data(paint.color_stops.colors));
        auto positions = inline_objects<float>(inline_data(paint.color_stops.positions));
        Vector<Gfx::ColorStop> color_stops;
        color_stops.ensure_capacity(colors.size());
        for (size_t i = 0; i < colors.size(); ++i)
            color_stops.unchecked_append({ .color = colors[i], .position = positions[i] });
        paint_style->set_color_stops(move(color_stops));
        return paint_style;
    };

    switch (paint.kind) {
    case Canvas2DPaintKind::Color:
        return MUST(Gfx::SolidColorPaintStyle::create(paint.color));
    case Canvas2DPaintKind::LinearGradient:
        return with_color_stops(MUST(Gfx::CanvasLinearGradientPaintStyle::create(paint.start_point, paint.end_point)));
    case Canvas2DPaintKind::RadialGradient:
        return with_color_stops(MUST(Gfx::CanvasRadialGradientPaintStyle::create(paint.start_point, paint.start_radius, paint.end_point, paint.end_radius)));
    case Canvas2DPaintKind::ConicGradient:
        return with_color_stops(MUST(Gfx::CanvasConicGradientPaintStyle::create(paint.start_point, paint.start_angle)));
    case Canvas2DPaintKind::Pattern: {
        Optional<Gfx::DecodedImageFrame> image;
        if (paint.pattern_has_image) {
            auto const* frame = m_resolvers.image_frame(paint.pattern_image_id.value());
            if (!frame)
                return nullptr;
            image = *frame;
        }
        auto repetition = static_cast<Gfx::CanvasPatternPaintStyle::Repetition>(to_underlying(paint.pattern_repetition));
        auto paint_style = MUST(Gfx::CanvasPatternPaintStyle::create(move(image), repetition));
        if (paint.pattern_transform.has_value())
            paint_style->set_transform(*paint.pattern_transform);
        return paint_style;
    }
    }
    VERIFY_NOT_REACHED();
}

void Canvas2DCommandPlayer::play_command(Canvas2DFillPath const& command)
{
    auto path = path_from(command.path_data);
    auto paint_style = paint_style_from(command.paint);
    if (!paint_style)
        return;
    m_painter->fill_path(path, *paint_style, image_filter_from(command.compositing), command.compositing.global_alpha, command.compositing.compositing_and_blending_operator, command.winding_rule, shadow_from(command.compositing));
}

void Canvas2DCommandPlayer::play_command(Canvas2DStrokePath const& command)
{
    auto path = path_from(command.path_data);
    auto dash_array = inline_objects<float>(inline_data(command.dash_array));
    auto paint_style = paint_style_from(command.paint);
    if (!paint_style)
        return;
    m_painter->stroke_path(path, *paint_style, image_filter_from(command.compositing), command.thickness, command.compositing.global_alpha, command.compositing.compositing_and_blending_operator, command.cap_style, command.join_style, command.miter_limit, dash_array, command.dash_offset, shadow_from(command.compositing));
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawGlyphRun const& command)
{
    if (!m_resolvers.font)
        return;
    auto const* font = m_resolvers.font(command.font_id.value());
    if (!font)
        return;
    auto sk_font = skia_font(*font, 1);
    if (!sk_font.has_value())
        return;
    auto paint_style = paint_style_from(command.paint);
    if (!paint_style)
        return;
    auto tape_glyphs = inline_objects<DisplayListGlyph>(inline_data(command.glyphs));
    Vector<Gfx::CanvasGlyph> glyphs;
    glyphs.ensure_capacity(tape_glyphs.size());
    for (auto const& glyph : tape_glyphs)
        glyphs.unchecked_append({ .position = glyph.position, .glyph_id = glyph.glyph_id });
    m_painter->draw_glyph_run(*font, sk_font.release_value(), glyphs, command.translation, *paint_style, image_filter_from(command.compositing), command.compositing.global_alpha, command.compositing.compositing_and_blending_operator, shadow_from(command.compositing));
}

void Canvas2DCommandPlayer::play_command(Canvas2DClearRect const& command)
{
    m_painter->clear_rect(command.rect, command.color);
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawImageFrame const& command)
{
    auto const* frame = m_resolvers.image_frame(command.frame_id.value());
    if (!frame)
        return;
    m_painter->draw_bitmap(command.dst_rect, *frame, command.src_rect, command.scaling_mode, image_filter_from(command.compositing), command.compositing.global_alpha, command.compositing.compositing_and_blending_operator, shadow_from(command.compositing));
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawCanvas const& command)
{
    if (!m_resolvers.canvas_surface)
        return;

    auto* source_surface = m_resolvers.canvas_surface(command.source_canvas_id.value());
    if (!source_surface)
        return;

    auto image = source_surface->sk_image_snapshot<sk_sp<SkImage>>();
    if (!image)
        return;
    draw_image(*image, command.dst_rect, command.src_rect, command.scaling_mode, command.compositing);
}

void Canvas2DCommandPlayer::play_command(Canvas2DDrawVideoFrame const& command)
{
    if (!m_resolvers.video_frame_image)
        return;
    auto image = m_resolvers.video_frame_image(command.video_sink_handle, command.generation);
    if (!image)
        return;
    draw_image(*image, command.dst_rect, command.src_rect, command.scaling_mode, command.compositing);
}

void Canvas2DCommandPlayer::draw_image(SkImage const& image, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Canvas2DCompositing const& compositing)
{
    m_painter->draw_image(image, dst_rect, src_rect, scaling_mode, image_filter_from(compositing), compositing.global_alpha, compositing.compositing_and_blending_operator, shadow_from(compositing));
}

void Canvas2DCommandPlayer::play_command(Canvas2DSetTransform const& command)
{
    m_painter->set_transform(command.transform);
}

void Canvas2DCommandPlayer::play_command(Canvas2DSave const&)
{
    m_painter->save();
}

void Canvas2DCommandPlayer::play_command(Canvas2DRestore const&)
{
    m_painter->restore();
}

void Canvas2DCommandPlayer::play_command(Canvas2DClipPath const& command)
{
    m_painter->clip(path_from(command.path_data), command.winding_rule);
}

void Canvas2DCommandPlayer::play_command(Canvas2DReset const&)
{
    m_painter->reset();
}

void Canvas2DCommandPlayer::play_command(Canvas2DClearCanvas const& command)
{
    auto size = m_surface->size();
    if (size.is_empty())
        return;

    static constexpr int max_strip_height = 64;
    auto strip_height = min(size.height(), max_strip_height);
    auto strip_or_error = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { size.width(), strip_height });
    if (strip_or_error.is_error())
        return;
    auto strip = strip_or_error.release_value();
    auto alpha = command.color.alpha();
    auto premultiply = [alpha](u8 channel) { return static_cast<u8>((channel * alpha + 127) / 255); };
    auto pixel = Gfx::Color(premultiply(command.color.red()), premultiply(command.color.green()), premultiply(command.color.blue()), alpha).value();
    for (int y = 0; y < strip_height; ++y) {
        auto* scanline = strip->scanline(y);
        for (int x = 0; x < size.width(); ++x)
            scanline[x] = pixel;
    }

    for (int y = 0; y < size.height(); y += strip_height)
        m_surface->write_from_bitmap(*strip, { 0, y }, m_color_space);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata
void Canvas2DCommandPlayer::play_command(Canvas2DPutImageFrame const& command)
{
    // NB: putImageData() sets the pixels of the output bitmap, so they are written to the surface directly, without
    //     the current transformation matrix, the clipping region, or compositing.
    auto const* frame = m_resolvers.image_frame(command.frame_id.value());
    if (!frame)
        return;
    m_surface->write_from_bitmap(frame->bitmap(), command.position, frame->color_space());
}

}
