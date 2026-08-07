/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/PausedDebuggerOverlay.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWebView/PausedDebuggerOverlay.h>
#include <core/SkCanvas.h>
#include <core/SkFont.h>
#include <core/SkFontMetrics.h>
#include <core/SkPaint.h>
#include <core/SkPathBuilder.h>
#include <core/SkRRect.h>

namespace Compositor {

void paint_paused_debugger_overlay(Gfx::PaintingSurface& surface, Gfx::IntSize viewport_size, double device_pixel_ratio, Optional<String> const& font_family, Optional<WebView::PausedDebuggerOverlayAction> hovered_action)
{
    // These colors are equivalent to those produced by UI/Qt/ChromeStyle.cpp in light mode. The compositor cannot use
    // those colors directly because it does not depend on Qt, so we have to copy their resolved values here.
    static constexpr auto overlay_color = Color(0xee, 0xee, 0xef, 168);
    static constexpr auto toolbar_color = Color(0xff, 0xff, 0xff);
    static constexpr auto toolbar_border_color = Color(0xce, 0xce, 0xcf);
    static constexpr auto button_hover_color = Color(0xf1, 0xf1, 0xf2);
    static constexpr auto text_color = Color(0x18, 0x1d, 0x24);
    static constexpr auto shadow_color = Color(0x18, 0x1d, 0x24, 66);

    auto& canvas = surface.canvas();
    auto geometry = WebView::paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio);
    auto radius = max(1.0f, static_cast<float>(4 * device_pixel_ratio));

    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(Gfx::to_skia_color(overlay_color));
    canvas.drawRect(SkRect::MakeWH(viewport_size.width(), viewport_size.height()), paint);

    auto shadow_rect = geometry.toolbar.translated(0, max(1, static_cast<int>(round(2 * device_pixel_ratio))));
    paint.setColor(Gfx::to_skia_color(shadow_color));
    canvas.drawRRect(SkRRect::MakeRectXY(Gfx::to_skia_rect(shadow_rect), radius, radius), paint);

    paint.setColor(Gfx::to_skia_color(toolbar_color));
    canvas.drawRRect(SkRRect::MakeRectXY(Gfx::to_skia_rect(geometry.toolbar), radius, radius), paint);

    if (hovered_action.has_value()) {
        auto const& hovered_button = *hovered_action == WebView::PausedDebuggerOverlayAction::StepOver
            ? geometry.step_over_button
            : geometry.continue_button;
        canvas.save();
        canvas.clipRRect(SkRRect::MakeRectXY(Gfx::to_skia_rect(geometry.toolbar), radius, radius), true);
        paint.setColor(Gfx::to_skia_color(button_hover_color));
        canvas.drawRect(Gfx::to_skia_rect(hovered_button), paint);
        canvas.restore();
    }

    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(max(1.0, device_pixel_ratio));
    paint.setColor(Gfx::to_skia_color(toolbar_border_color));
    canvas.drawRRect(SkRRect::MakeRectXY(Gfx::to_skia_rect(geometry.toolbar), radius, radius), paint);
    paint.setStyle(SkPaint::kFill_Style);

    auto divider_inset = max(1, static_cast<int>(round(10 * device_pixel_ratio)));
    auto divider_x = geometry.step_over_button.left();
    canvas.drawLine(
        divider_x,
        geometry.toolbar.top() + divider_inset,
        divider_x,
        geometry.toolbar.bottom() - divider_inset,
        paint);

    RefPtr<Gfx::Font> font;
    if (font_family.has_value())
        font = Gfx::FontDatabase::the().get(FlyString { *font_family }, 12, 400, Gfx::FontWidth::Normal, 0);
    if (!font)
        font = Gfx::FontDatabase::the().get("SerenitySans"_fly_string, 12, 400, Gfx::FontWidth::Normal, 0);
    paint.setColor(Gfx::to_skia_color(text_color));
    if (font) {
        auto skia_font = font->skia_font(static_cast<float>(device_pixel_ratio));
        static constexpr auto label = "Paused in debugger"sv;
        auto label_width = skia_font.measureText(label.characters_without_null_termination(), label.length(), SkTextEncoding::kUTF8);
        SkFontMetrics font_metrics;
        skia_font.getMetrics(&font_metrics);
        auto label_x = geometry.message.x() + (geometry.message.width() - label_width) / 2;
        auto label_y = geometry.message.center().y() - (font_metrics.fAscent + font_metrics.fDescent) / 2;
        canvas.drawSimpleText(label.characters_without_null_termination(), label.length(), SkTextEncoding::kUTF8, label_x, label_y, skia_font, paint);
    }

    auto icon_size = max(8.0, 16 * device_pixel_ratio);
    auto line_thickness = max(1.0, device_pixel_ratio);

    auto paint_continue_icon = [&](Gfx::IntRect const& button_rect) {
        auto icon_left = button_rect.center().x() - icon_size / 2;
        auto icon_top = button_rect.center().y() - icon_size / 2;
        SkPathBuilder path;
        path.moveTo(icon_left + icon_size / 4, icon_top + icon_size / 8);
        path.lineTo(icon_left + icon_size / 4, icon_top + icon_size - icon_size / 8);
        path.lineTo(icon_left + icon_size - icon_size / 8, icon_top + icon_size / 2);
        path.close();
        canvas.drawPath(path.detach(), paint);
    };

    auto paint_step_over_icon = [&](Gfx::IntRect const& button_rect) {
        auto center = button_rect.center();
        auto left = center.x() - icon_size / 2;
        auto top = center.y() - icon_size / 2;
        auto right = center.x() + icon_size / 2;

        SkPathBuilder arc;
        arc.moveTo(left + icon_size / 8, top + icon_size * 0.55);
        arc.cubicTo(
            left + icon_size / 8, top + icon_size * 0.15,
            right - icon_size / 5, top + icon_size * 0.15,
            right - icon_size / 5, top + icon_size * 0.55);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeCap(SkPaint::kRound_Cap);
        paint.setStrokeJoin(SkPaint::kRound_Join);
        paint.setStrokeWidth(line_thickness);
        canvas.drawPath(arc.detach(), paint);
        paint.setStyle(SkPaint::kFill_Style);

        SkPathBuilder arrow;
        auto arrow_width = icon_size / 2.5;
        arrow.moveTo(right - arrow_width, top + icon_size * 0.55);
        arrow.lineTo(right, top + icon_size * 0.55);
        arrow.lineTo(right - arrow_width / 2, top + icon_size * 0.8);
        arrow.close();
        canvas.drawPath(arrow.detach(), paint);

        auto dot_size = max(2.0, line_thickness * 2);
        canvas.drawCircle(center.x(), center.y() + icon_size / 3, dot_size / 2, paint);
    };

    paint_step_over_icon(geometry.step_over_button);
    paint_continue_icon(geometry.continue_button);
}

}
