/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/PaintingSurface.h>
#include <Compositor/PictureInPictureControls.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/UIOverlays.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkPathBuilder.h>

namespace Compositor {

void paint_picture_in_picture_controls(Gfx::PaintingSurface& surface, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::PictureInPictureControlsState const& state, float opacity)
{
    static constexpr auto button_color = Color(0, 0, 0, 115);
    static constexpr auto icon_color = Color(0xff, 0xff, 0xff);
    static constexpr auto hovered_icon_color = Color(0xe0, 0xd4, 0xff);

    auto& canvas = surface.canvas();
    auto geometry = Compositing::picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio);
    auto bounds = Gfx::to_skia_rect(Compositing::overlay_buttons_bounds(geometry.buttons()));
    canvas.saveLayerAlphaf(&bounds, opacity);

    SkPaint paint;
    paint.setAntiAlias(true);

    // The icons are stroked on a grid of 16 units, and drawn at the given size in points.
    auto paint_button = [&](Gfx::IntRect const& button, Compositing::PictureInPictureControl control, SkPathBuilder& icon, float icon_points, float stroke_units) {
        auto center = button.to_type<float>().center();
        paint.setStyle(SkPaint::kFill_Style);
        paint.setColor(Gfx::to_skia_color(button_color));
        canvas.drawCircle(center.x(), center.y(), button.width() / 2.0f, paint);

        auto icon_size = static_cast<float>(icon_points * device_pixel_ratio);
        canvas.save();
        canvas.translate(center.x() - icon_size / 2, center.y() - icon_size / 2);
        canvas.scale(icon_size / 16, icon_size / 16);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(stroke_units);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        paint.setColor(Gfx::to_skia_color(state.hovered_control == control ? hovered_icon_color : icon_color));
        canvas.drawPath(icon.detach(), paint);
        canvas.restore();
    };

    // An arrow out of a box, toward its top right corner.
    SkPathBuilder back_to_tab_icon;
    back_to_tab_icon.moveTo(9.5f, 2.5f);
    back_to_tab_icon.lineTo(13.5f, 2.5f);
    back_to_tab_icon.lineTo(13.5f, 6.5f);
    back_to_tab_icon.moveTo(13.5f, 2.5f);
    back_to_tab_icon.lineTo(8, 8);
    back_to_tab_icon.moveTo(12, 9.5f);
    back_to_tab_icon.lineTo(12, 12.5f);
    back_to_tab_icon.quadTo(12, 13.5f, 11, 13.5f);
    back_to_tab_icon.lineTo(3.5f, 13.5f);
    back_to_tab_icon.quadTo(2.5f, 13.5f, 2.5f, 12.5f);
    back_to_tab_icon.lineTo(2.5f, 5);
    back_to_tab_icon.quadTo(2.5f, 4, 3.5f, 4);
    back_to_tab_icon.lineTo(6.5f, 4);
    paint_button(geometry.back_to_tab_button, Compositing::PictureInPictureControl::BackToTab, back_to_tab_icon, 14, 1.6f);

    SkPathBuilder close_icon;
    close_icon.moveTo(4, 4);
    close_icon.lineTo(12, 12);
    close_icon.moveTo(12, 4);
    close_icon.lineTo(4, 12);
    paint_button(geometry.close_button, Compositing::PictureInPictureControl::Close, close_icon, 11, 2);

    canvas.restore();
}

}
