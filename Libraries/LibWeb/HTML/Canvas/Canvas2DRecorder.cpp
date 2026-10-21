/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Noncopyable.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibCompositing/DisplayList/DisplayListCommandsGenerated.h>
#include <LibCompositing/RustFFI.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/HTML/Canvas/Canvas2DRecorder.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>

namespace Web::HTML {

using Compositing::RustFFI::FfiCanvas2DCompositing;
using Compositing::RustFFI::FfiCanvas2DPaint;
using Compositing::RustFFI::FfiCanvas2DStroke;

// NB: `AlphaType` of display_list/canvas_2d_recorder.rs is a u8 with the values of Gfx::AlphaType.
static_assert(to_underlying(Gfx::AlphaType::Premultiplied) == 0);
static_assert(to_underlying(Gfx::AlphaType::Unpremultiplied) == 1);

static_assert(to_underlying(Compositing::Canvas2DPatternRepetition::Repeat) == to_underlying(Gfx::CanvasPatternPaintStyle::Repetition::Repeat));
static_assert(to_underlying(Compositing::Canvas2DPatternRepetition::RepeatX) == to_underlying(Gfx::CanvasPatternPaintStyle::Repetition::RepeatX));
static_assert(to_underlying(Compositing::Canvas2DPatternRepetition::RepeatY) == to_underlying(Gfx::CanvasPatternPaintStyle::Repetition::RepeatY));
static_assert(to_underlying(Compositing::Canvas2DPatternRepetition::NoRepeat) == to_underlying(Gfx::CanvasPatternPaintStyle::Repetition::NoRepeat));

// The C ABI of Libraries/LibWeb/Bindings/Rust/src/canvas_2d/exports.rs.
extern "C" {
RustCanvas2DRecorder* web_canvas_2d_recorder_create(void* transport);
void web_canvas_2d_recorder_destroy(RustCanvas2DRecorder*);
bool web_canvas_2d_recorder_create_context(RustCanvas2DRecorder*, i32 width, i32 height, bool alpha);
bool web_canvas_2d_recorder_canvas_id(RustCanvas2DRecorder const*, u64* out_canvas_id);
void web_canvas_2d_recorder_destroy_context(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_flush(RustCanvas2DRecorder*);
void* web_canvas_2d_recorder_read_back_pixels(RustCanvas2DRecorder*, Gfx::IntRect rect, u8 alpha_type);
void web_canvas_2d_recorder_present(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_prepare_to_record(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_fill_path(RustCanvas2DRecorder*, ReadonlyBytes path, FfiCanvas2DPaint const*, Gfx::WindingRule, float blur_radius, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_stroke_path(RustCanvas2DRecorder*, ReadonlyBytes path, FfiCanvas2DPaint const*, FfiCanvas2DStroke const*, float blur_radius, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_draw_glyph_run(RustCanvas2DRecorder*, void const* font, ReadonlySpan<Compositing::DisplayListGlyph>, Gfx::FloatPoint translation, FfiCanvas2DPaint const*, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_clear_rect(RustCanvas2DRecorder*, Gfx::FloatRect rect, Gfx::Color);
void web_canvas_2d_recorder_draw_image_frame(RustCanvas2DRecorder*, void const* frame, Gfx::FloatRect dst_rect, Gfx::IntRect src_rect, Gfx::ScalingMode, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_draw_canvas(RustCanvas2DRecorder*, u64 source_canvas_id, Gfx::FloatRect dst_rect, Gfx::IntRect src_rect, Gfx::ScalingMode, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_draw_video_frame(RustCanvas2DRecorder*, void* hold, u64 video_sink_handle, u64 generation, Gfx::FloatRect dst_rect, Gfx::IntRect src_rect, Gfx::ScalingMode, FfiCanvas2DCompositing const*);
void web_canvas_2d_recorder_set_transform(RustCanvas2DRecorder*, Gfx::AffineTransform const*);
void web_canvas_2d_recorder_save(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_restore(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_clip_path(RustCanvas2DRecorder*, ReadonlyBytes path, Gfx::WindingRule);
void web_canvas_2d_recorder_reset(RustCanvas2DRecorder*);
void web_canvas_2d_recorder_clear_canvas(RustCanvas2DRecorder*, Gfx::Color);

// Defined in Bindings/Rust/src/html/exports.rs.
void web_canvas_gradient_lend_paint(void* gradient, FfiCanvas2DPaint* out_paint);
}

namespace {

// A fill or stroke style as the recorder borrows it, with the pattern image it lends kept alive. A gradient lends its
// color stops, which do not change while the recorder borrows them.
class LentPaint {
    AK_MAKE_NONCOPYABLE(LentPaint);
    AK_MAKE_NONMOVABLE(LentPaint);

public:
    explicit LentPaint(Gfx::Color color)
    {
        m_paint.kind = Compositing::Canvas2DPaintKind::Color;
        m_paint.color = color;
    }

    explicit LentPaint(FillOrStrokeStyle const& style)
    {
        style.variant().visit(
            [&](Gfx::Color color) {
                m_paint.kind = Compositing::Canvas2DPaintKind::Color;
                m_paint.color = color;
            },
            [&](GC::Ref<CanvasGradient> gradient) {
                web_canvas_gradient_lend_paint(gradient.ptr(), &m_paint);
            },
            [&](GC::Ref<CanvasPattern> canvas_pattern) {
                auto paint_style = canvas_pattern->to_gfx_paint_style();
                auto const& pattern = as<Gfx::CanvasPatternPaintStyle>(*paint_style);
                m_paint.kind = Compositing::Canvas2DPaintKind::Pattern;
                m_paint.pattern_repetition = static_cast<Compositing::Canvas2DPatternRepetition>(to_underlying(pattern.repetition()));
                m_pattern_image = pattern.image();
                if (m_pattern_image.has_value())
                    m_paint.pattern_image = &m_pattern_image.value();
                if (auto const& transform = pattern.transform(); transform.has_value()) {
                    m_paint.has_pattern_transform = true;
                    m_paint.pattern_transform = *transform;
                }
            });
    }

    FfiCanvas2DPaint const* ptr() const { return &m_paint; }

private:
    FfiCanvas2DPaint m_paint {
        .kind = Compositing::Canvas2DPaintKind::Color,
        .pattern_repetition = Compositing::Canvas2DPatternRepetition::Repeat,
        .has_pattern_transform = false,
        .color = {},
        .start_point = {},
        .end_point = {},
        .start_radius = 0,
        .end_radius = 0,
        .start_angle = 0,
        .color_stop_colors = {},
        .color_stop_positions = {},
        .pattern_image = nullptr,
        .pattern_transform = {},
    };
    Optional<Gfx::DecodedImageFrame> m_pattern_image;
};

}

static FfiCanvas2DCompositing lend(Canvas2DCompositing const& compositing)
{
    return {
        .filter = compositing.filter ? compositing.filter->serialized_bytes() : ReadonlyBytes {},
        .has_filter = compositing.filter != nullptr,
        .global_alpha = compositing.global_alpha,
        .compositing_and_blending_operator = compositing.compositing_and_blending_operator,
    };
}

static FfiCanvas2DStroke lend(Canvas2DStroke const& stroke)
{
    return {
        .thickness = stroke.thickness,
        .cap_style = stroke.cap_style,
        .join_style = stroke.join_style,
        .miter_limit = stroke.miter_limit,
        .dash_array = stroke.dash_array,
        .dash_offset = stroke.dash_offset,
    };
}

NonnullOwnPtr<Canvas2DRecorder> Canvas2DRecorder::create(NonnullRefPtr<RemoteCanvas2DTransport> transport)
{
    return adopt_own(*new Canvas2DRecorder(web_canvas_2d_recorder_create(&transport.leak_ref())));
}

Canvas2DRecorder::Canvas2DRecorder(RustCanvas2DRecorder* recorder)
    : m_recorder(recorder)
{
    VERIFY(m_recorder);
}

Canvas2DRecorder::~Canvas2DRecorder()
{
    web_canvas_2d_recorder_destroy(m_recorder);
}

bool Canvas2DRecorder::create_context(Gfx::IntSize size, bool alpha)
{
    return web_canvas_2d_recorder_create_context(m_recorder, size.width(), size.height(), alpha);
}

Optional<Compositing::CanvasId> Canvas2DRecorder::canvas_id() const
{
    u64 canvas_id = 0;
    if (!web_canvas_2d_recorder_canvas_id(m_recorder, &canvas_id))
        return {};
    return Compositing::CanvasId { canvas_id };
}

void Canvas2DRecorder::destroy_context()
{
    web_canvas_2d_recorder_destroy_context(m_recorder);
}

void Canvas2DRecorder::flush()
{
    web_canvas_2d_recorder_flush(m_recorder);
}

RefPtr<Gfx::Bitmap> Canvas2DRecorder::read_back_pixels(Gfx::IntRect const& rect, Gfx::AlphaType alpha_type)
{
    auto* bitmap = web_canvas_2d_recorder_read_back_pixels(m_recorder, rect, to_underlying(alpha_type));
    if (!bitmap)
        return nullptr;
    return adopt_ref(*static_cast<Gfx::Bitmap*>(bitmap));
}

void Canvas2DRecorder::present()
{
    web_canvas_2d_recorder_present(m_recorder);
}

void Canvas2DRecorder::prepare_to_record()
{
    web_canvas_2d_recorder_prepare_to_record(m_recorder);
}

void Canvas2DRecorder::fill_path(Gfx::Path const& path, FillOrStrokeStyle const& style, Gfx::WindingRule winding_rule, Canvas2DCompositing const& compositing)
{
    auto path_bytes = path.serialize_to_bytes();
    LentPaint paint { style };
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_fill_path(m_recorder, path_bytes.span(), paint.ptr(), winding_rule, 0, &lent_compositing);
}

void Canvas2DRecorder::fill_path_shadow(Gfx::Path const& path, Gfx::Color color, Gfx::WindingRule winding_rule, float blur_radius, Canvas2DCompositing const& compositing)
{
    auto path_bytes = path.serialize_to_bytes();
    LentPaint paint { color };
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_fill_path(m_recorder, path_bytes.span(), paint.ptr(), winding_rule, blur_radius, &lent_compositing);
}

void Canvas2DRecorder::stroke_path(Gfx::Path const& path, FillOrStrokeStyle const& style, Canvas2DStroke const& stroke, Canvas2DCompositing const& compositing)
{
    auto path_bytes = path.serialize_to_bytes();
    LentPaint paint { style };
    auto lent_stroke = lend(stroke);
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_stroke_path(m_recorder, path_bytes.span(), paint.ptr(), &lent_stroke, 0, &lent_compositing);
}

void Canvas2DRecorder::stroke_path_shadow(Gfx::Path const& path, Gfx::Color color, Canvas2DStroke const& stroke, float blur_radius, Canvas2DCompositing const& compositing)
{
    auto path_bytes = path.serialize_to_bytes();
    LentPaint paint { color };
    auto lent_stroke = lend(stroke);
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_stroke_path(m_recorder, path_bytes.span(), paint.ptr(), &lent_stroke, blur_radius, &lent_compositing);
}

void Canvas2DRecorder::draw_glyph_run(Gfx::Font const& font, ReadonlySpan<Compositing::DisplayListGlyph> glyphs, Gfx::FloatPoint translation, FillOrStrokeStyle const& style, Canvas2DCompositing const& compositing)
{
    LentPaint paint { style };
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_draw_glyph_run(m_recorder, &font, glyphs, translation, paint.ptr(), &lent_compositing);
}

void Canvas2DRecorder::clear_rect(Gfx::FloatRect const& rect, Gfx::Color color)
{
    web_canvas_2d_recorder_clear_rect(m_recorder, rect, color);
}

void Canvas2DRecorder::draw_image_frame(Gfx::DecodedImageFrame const& frame, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Canvas2DCompositing const& compositing)
{
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_draw_image_frame(m_recorder, &frame, dst_rect, src_rect, scaling_mode, &lent_compositing);
}

void Canvas2DRecorder::draw_canvas(Compositing::CanvasId source_canvas_id, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Canvas2DCompositing const& compositing)
{
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_draw_canvas(m_recorder, source_canvas_id.value(), dst_rect, src_rect, scaling_mode, &lent_compositing);
}

void Canvas2DRecorder::draw_video_frame(Compositing::CapturedVideoFrame const& captured_frame, Gfx::FloatRect const& dst_rect, Gfx::IntRect const& src_rect, Gfx::ScalingMode scaling_mode, Canvas2DCompositing const& compositing)
{
    VERIFY(captured_frame.hold);
    auto lent_compositing = lend(compositing);
    web_canvas_2d_recorder_draw_video_frame(m_recorder, captured_frame.hold.ptr(), captured_frame.sink_handle.value(), captured_frame.generation, dst_rect, src_rect, scaling_mode, &lent_compositing);
}

void Canvas2DRecorder::set_transform(Gfx::AffineTransform const& transform)
{
    web_canvas_2d_recorder_set_transform(m_recorder, &transform);
}

void Canvas2DRecorder::save()
{
    web_canvas_2d_recorder_save(m_recorder);
}

void Canvas2DRecorder::restore()
{
    web_canvas_2d_recorder_restore(m_recorder);
}

void Canvas2DRecorder::clip_path(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto path_bytes = path.serialize_to_bytes();
    web_canvas_2d_recorder_clip_path(m_recorder, path_bytes.span(), winding_rule);
}

void Canvas2DRecorder::reset()
{
    web_canvas_2d_recorder_reset(m_recorder);
}

void Canvas2DRecorder::clear_canvas(Gfx::Color color)
{
    web_canvas_2d_recorder_clear_canvas(m_recorder, color);
}

}
