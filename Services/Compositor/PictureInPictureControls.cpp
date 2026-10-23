/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Span.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/PictureInPictureControls.h>
#include <Compositor/SkiaUtils.h>
#include <LibCompositing/UIOverlays.h>
#include <core/SkCanvas.h>
#include <core/SkPaint.h>
#include <core/SkPathBuilder.h>

namespace Compositor {

// The icons are drawn on a grid of 24 units.
static constexpr SkPoint back_to_tab_frame_outside[] = { { 3, 3 }, { 21, 3 }, { 21, 21 }, { 3, 21 } };
static constexpr SkPoint back_to_tab_frame_inside[] = { { 5, 5 }, { 5, 19 }, { 19, 19 }, { 19, 5 } };
static constexpr SkPoint back_to_tab_arrow[] = {
    { 16.6f, 6 },
    { 9, 13.6f },
    { 9, 9 },
    { 7, 9 },
    { 7, 17 },
    { 15, 17 },
    { 15, 15 },
    { 10.4f, 15 },
    { 18, 7.4f },
};
static constexpr SkPoint close_cross[] = {
    { 6.4f, 5 },
    { 12, 10.6f },
    { 17.6f, 5 },
    { 19, 6.4f },
    { 13.4f, 12 },
    { 19, 17.6f },
    { 17.6f, 19 },
    { 12, 13.4f },
    { 6.4f, 19 },
    { 5, 17.6f },
    { 10.6f, 12 },
    { 5, 6.4f },
};

static void add_polygon(SkPathBuilder& path, ReadonlySpan<SkPoint> points)
{
    path.moveTo(points.first());
    for (auto const& point : points.slice(1))
        path.lineTo(point);
    path.close();
}

void paint_picture_in_picture_controls(Gfx::PaintingSurface& surface, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::PictureInPictureControlsState const& state, float opacity)
{
    static constexpr auto button_color = Color(0, 0, 0, 153);
    static constexpr auto icon_color = Color(0xff, 0xff, 0xff);
    static constexpr auto hovered_icon_color = Color(0xe0, 0xd4, 0xff);

    auto& canvas = surface.canvas();
    auto geometry = Compositing::picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio);
    auto bounds = Gfx::to_skia_rect(Compositing::overlay_buttons_bounds(geometry.buttons()));
    canvas.saveLayerAlphaf(&bounds, opacity);

    SkPaint paint;
    paint.setAntiAlias(true);

    auto paint_button = [&](Gfx::IntRect const& button, Compositing::PictureInPictureControl control, SkPathBuilder& icon) {
        auto center = button.to_type<float>().center();
        paint.setColor(Gfx::to_skia_color(button_color));
        canvas.drawCircle(center.x(), center.y(), button.width() / 2.0f, paint);

        auto icon_size = static_cast<float>(20 * device_pixel_ratio);
        canvas.save();
        canvas.translate(center.x() - icon_size / 2, center.y() - icon_size / 2);
        canvas.scale(icon_size / 24, icon_size / 24);
        paint.setColor(Gfx::to_skia_color(state.hovered_control == control ? hovered_icon_color : icon_color));
        canvas.drawPath(icon.detach(), paint);
        canvas.restore();
    };

    SkPathBuilder back_to_tab_icon { SkPathFillType::kEvenOdd };
    add_polygon(back_to_tab_icon, back_to_tab_frame_outside);
    add_polygon(back_to_tab_icon, back_to_tab_frame_inside);
    add_polygon(back_to_tab_icon, back_to_tab_arrow);
    paint_button(geometry.back_to_tab_button, Compositing::PictureInPictureControl::BackToTab, back_to_tab_icon);

    SkPathBuilder close_icon;
    add_polygon(close_icon, close_cross);
    paint_button(geometry.close_button, Compositing::PictureInPictureControl::Close, close_icon);

    canvas.restore();
}

}
