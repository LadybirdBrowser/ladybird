/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibPaintServer/Compositor/DrawCommands.h>
#include <LibPaintServer/Compositor/DrawList.h>
#include <LibPaintServer/Types.h>
#include <PaintServer/FontResourceCache.h>
#include <core/SkRefCnt.h>

class SkCanvas;
class SkImage;
class SkImageFilter;

namespace PaintServer {

using DrawScaledImagePainter = Function<ErrorOr<void>(DrawScaledImageCommand const&, SkCanvas&, sk_sp<SkImageFilter> const&)>;
using DrawExternalContentPainter = Function<ErrorOr<void>(DrawExternalContentCommand const&, SkCanvas&)>;
using DrawRepeatedImagePainter = Function<ErrorOr<void>(DrawRepeatedImageCommand const&, SkCanvas&)>;
using DrawImageResolver = Function<sk_sp<SkImage>(ResourceID, ImageID)>;
using ScrollOffsetResolver = Function<Gfx::FloatPoint(u32)>;

struct DrawContext {
    ConnectionID connection_id { 0 };
    SurfaceID surface_id { 0 };
    FontResourceCache& font_cache;
    DrawScaledImagePainter const* draw_scaled_image_painter { nullptr };
    DrawExternalContentPainter const* draw_external_content_painter { nullptr };
    DrawRepeatedImagePainter const* draw_repeated_image_painter { nullptr };
    DrawImageResolver const* image_resolver { nullptr };
    ScrollOffsetResolver const* scroll_offset_resolver { nullptr };
};

class DrawCommandPlayer {
public:
    DrawCommandPlayer(DrawContext const&, SkCanvas&);

    ErrorOr<void> apply(DrawCommandView const& command_view);

private:
    ErrorOr<void> apply_clear_rect(DrawCommandView const& command_view);
    ErrorOr<void> apply_fill_rect(DrawCommandView const& command_view);
    ErrorOr<void> apply_fill_rect_with_rounded_corners(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_scaled_image(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_external_content(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_rect(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_line(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_ellipse(DrawCommandView const& command_view);
    ErrorOr<void> apply_fill_ellipse(DrawCommandView const& command_view);
    ErrorOr<void> apply_save(DrawCommandView const& command_view);
    ErrorOr<void> apply_save_layer(DrawCommandView const& command_view);
    ErrorOr<void> apply_restore(DrawCommandView const& command_view);
    ErrorOr<void> apply_reset_canvas_state(DrawCommandView const& command_view);
    ErrorOr<void> apply_translate(DrawCommandView const& command_view);
    ErrorOr<void> apply_add_clip_rect(DrawCommandView const& command_view);
    ErrorOr<void> apply_add_clip_path(DrawCommandView const& command_view);
    ErrorOr<void> apply_effects(DrawCommandView const& command_view);
    ErrorOr<void> apply_backdrop_filter(DrawCommandView const& command_view);
    ErrorOr<void> apply_set_transform(DrawCommandView const& command_view);
    ErrorOr<void> apply_transform(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_glyph_run(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_linear_gradient(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_outer_box_shadow(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_inner_box_shadow(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_text_shadow(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_scroll_bar(DrawCommandView const& command_view);
    ErrorOr<void> apply_fill_path(DrawCommandView const& command_view);
    ErrorOr<void> apply_stroke_path(DrawCommandView const& command_view);
    ErrorOr<void> apply_add_rounded_rect_clip(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_radial_gradient(DrawCommandView const& command_view);
    ErrorOr<void> apply_paint_conic_gradient(DrawCommandView const& command_view);
    ErrorOr<void> apply_draw_repeated_image(DrawCommandView const& command_view);

    ErrorOr<void> draw_glyph_run(DrawGlyphRunCommand const& header, ReadonlySpan<Glyph const> glyphs);
    void maybe_draw_magenta_rect(DrawCommandView const& command_view);

    DrawContext const& m_draw_context;
    SkCanvas& m_canvas;
    int m_initial_save_count { 1 };
};

}
