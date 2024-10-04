/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021, Mustafa Quraish <mustafa@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@gmta.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DeprecatedPainter.h"
#include "Bitmap.h"
#include "Font/Font.h"
#include <AK/Assertions.h>
#include <AK/Function.h>
#include <AK/Math.h>
#include <AK/Memory.h>
#include <AK/Stack.h>
#include <AK/StdLibExtras.h>
#include <AK/Utf8View.h>
#include <LibGfx/DeprecatedPath.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Quad.h>
#include <LibGfx/TextLayout.h>
#include <stdio.h>

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

namespace Gfx {

template<BitmapFormat format = BitmapFormat::Invalid>
ALWAYS_INLINE Color get_pixel(Gfx::Bitmap const& bitmap, int x, int y)
{
    if constexpr (format == BitmapFormat::BGRx8888)
        return Color::from_rgb(bitmap.scanline(y)[x]);
    if constexpr (format == BitmapFormat::BGRA8888)
        return Color::from_argb(bitmap.scanline(y)[x]);
    return bitmap.get_pixel(x, y);
}

DeprecatedPainter::DeprecatedPainter(Gfx::Bitmap& bitmap)
    : m_target(bitmap)
{
    VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRx8888 || bitmap.format() == Gfx::BitmapFormat::BGRA8888);
    m_state_stack.append(State());
    state().clip_rect = { { 0, 0 }, bitmap.size() };
    m_clip_origin = state().clip_rect;
}

void DeprecatedPainter::clear_rect(IntRect const& a_rect, Color color)
{
    auto rect = a_rect.translated(translation()).intersected(clip_rect());
    if (rect.is_empty())
        return;

    VERIFY(target().rect().contains(rect));

    ARGB32* dst = target().scanline(rect.top()) + rect.left();
    size_t const dst_skip = target().pitch() / sizeof(ARGB32);

    for (int i = rect.height() - 1; i >= 0; --i) {
        fast_u32_fill(dst, color.value(), rect.width());
        dst += dst_skip;
    }
}

void DeprecatedPainter::fill_physical_rect(IntRect const& physical_rect, Color color)
{
    // Callers must do clipping.
    ARGB32* dst = target().scanline(physical_rect.top()) + physical_rect.left();
    size_t const dst_skip = target().pitch() / sizeof(ARGB32);

    auto dst_format = target().format();
    for (int i = physical_rect.height() - 1; i >= 0; --i) {
        for (int j = 0; j < physical_rect.width(); ++j)
            dst[j] = color_for_format(dst_format, dst[j]).blend(color).value();
        dst += dst_skip;
    }
}

void DeprecatedPainter::fill_rect(IntRect const& a_rect, Color color)
{
    if (color.alpha() == 0)
        return;

    if (color.alpha() == 0xff) {
        clear_rect(a_rect, color);
        return;
    }

    auto rect = a_rect.translated(translation()).intersected(clip_rect());
    if (rect.is_empty())
        return;
    VERIFY(target().rect().contains(rect));

    fill_physical_rect(rect, color);
}

void DeprecatedPainter::fill_rect(IntRect const& rect, PaintStyle const& paint_style)
{
    auto a_rect = rect.translated(translation());
    auto clipped_rect = a_rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;
    auto start_offset = clipped_rect.location() - a_rect.location();
    paint_style.paint(a_rect, [&](PaintStyle::SamplerFunction sample) {
        for (int y = 0; y < clipped_rect.height(); ++y) {
            for (int x = 0; x < clipped_rect.width(); ++x) {
                IntPoint point(x, y);
                set_physical_pixel(point + clipped_rect.location(), sample(point + start_offset), true);
            }
        }
    });
}

void DeprecatedPainter::fill_rect_with_gradient(Orientation orientation, IntRect const& a_rect, Color gradient_start, Color gradient_end)
{
    if (gradient_start == gradient_end) {
        fill_rect(a_rect, gradient_start);
        return;
    }
    return fill_rect_with_linear_gradient(a_rect, Array { ColorStop { gradient_start, 0 }, ColorStop { gradient_end, 1 } }, orientation == Orientation::Horizontal ? 90.0f : 0.0f);
}

void DeprecatedPainter::fill_rect_with_gradient(IntRect const& a_rect, Color gradient_start, Color gradient_end)
{
    return fill_rect_with_gradient(Orientation::Horizontal, a_rect, gradient_start, gradient_end);
}

void DeprecatedPainter::fill_rect_with_rounded_corners(IntRect const& a_rect, Color color, int radius)
{
    return fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius);
}

void DeprecatedPainter::fill_rect_with_rounded_corners(IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    // Fasttrack for rects without any border radii
    if (!top_left_radius && !top_right_radius && !bottom_right_radius && !bottom_left_radius)
        return fill_rect(a_rect, color);

    // Fully transparent, dont care.
    if (color.alpha() == 0)
        return;

    // FIXME: Allow for elliptically rounded corners
    IntRect top_left_corner = {
        a_rect.x(),
        a_rect.y(),
        top_left_radius,
        top_left_radius
    };
    IntRect top_right_corner = {
        a_rect.x() + a_rect.width() - top_right_radius,
        a_rect.y(),
        top_right_radius,
        top_right_radius
    };
    IntRect bottom_right_corner = {
        a_rect.x() + a_rect.width() - bottom_right_radius,
        a_rect.y() + a_rect.height() - bottom_right_radius,
        bottom_right_radius,
        bottom_right_radius
    };
    IntRect bottom_left_corner = {
        a_rect.x(),
        a_rect.y() + a_rect.height() - bottom_left_radius,
        bottom_left_radius,
        bottom_left_radius
    };

    IntRect top_rect = {
        a_rect.x() + top_left_radius,
        a_rect.y(),
        a_rect.width() - top_left_radius - top_right_radius, top_left_radius
    };
    IntRect right_rect = {
        a_rect.x() + a_rect.width() - top_right_radius,
        a_rect.y() + top_right_radius,
        top_right_radius,
        a_rect.height() - top_right_radius - bottom_right_radius
    };
    IntRect bottom_rect = {
        a_rect.x() + bottom_left_radius,
        a_rect.y() + a_rect.height() - bottom_right_radius,
        a_rect.width() - bottom_left_radius - bottom_right_radius,
        bottom_right_radius
    };
    IntRect left_rect = {
        a_rect.x(),
        a_rect.y() + top_left_radius,
        bottom_left_radius,
        a_rect.height() - top_left_radius - bottom_left_radius
    };

    IntRect inner = {
        left_rect.x() + left_rect.width(),
        left_rect.y(),
        a_rect.width() - left_rect.width() - right_rect.width(),
        a_rect.height() - top_rect.height() - bottom_rect.height()
    };

    fill_rect(top_rect, color);
    fill_rect(right_rect, color);
    fill_rect(bottom_rect, color);
    fill_rect(left_rect, color);

    fill_rect(inner, color);

    if (top_left_radius)
        fill_rounded_corner(top_left_corner, top_left_radius, color, CornerOrientation::TopLeft);
    if (top_right_radius)
        fill_rounded_corner(top_right_corner, top_right_radius, color, CornerOrientation::TopRight);
    if (bottom_left_radius)
        fill_rounded_corner(bottom_left_corner, bottom_left_radius, color, CornerOrientation::BottomLeft);
    if (bottom_right_radius)
        fill_rounded_corner(bottom_right_corner, bottom_right_radius, color, CornerOrientation::BottomRight);
}

void DeprecatedPainter::fill_rounded_corner(IntRect const& a_rect, int radius, Color color, CornerOrientation orientation)
{
    // Care about clipping
    auto translated_a_rect = a_rect.translated(translation());
    auto rect = translated_a_rect.intersected(clip_rect());

    if (rect.is_empty())
        return;
    VERIFY(target().rect().contains(rect));

    // We got cut on the top!
    // FIXME: Also account for clipping on the x-axis
    int clip_offset = 0;
    if (translated_a_rect.y() < rect.y())
        clip_offset = rect.y() - translated_a_rect.y();

    ARGB32* dst = target().scanline(rect.top()) + rect.left();
    size_t const dst_skip = target().pitch() / sizeof(ARGB32);

    IntPoint circle_center;
    switch (orientation) {
    case CornerOrientation::TopLeft:
        circle_center = { radius, radius + 1 };
        break;
    case CornerOrientation::TopRight:
        circle_center = { -1, radius + 1 };
        break;
    case CornerOrientation::BottomRight:
        circle_center = { -1, 0 };
        break;
    case CornerOrientation::BottomLeft:
        circle_center = { radius, 0 };
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    int radius2 = radius * radius;
    auto is_in_circle = [&](int x, int y) {
        int distance2 = (circle_center.x() - x) * (circle_center.x() - x) + (circle_center.y() - y) * (circle_center.y() - y);
        // To reflect the grid and be compatible with the draw_circle_arc_intersecting algorithm
        // add 1/2 to the radius
        return distance2 <= (radius2 + radius + 0.25);
    };

    auto dst_format = target().format();
    for (int i = rect.height() - 1; i >= 0; --i) {
        for (int j = 0; j < rect.width(); ++j)
            if (is_in_circle(j, rect.height() - i + clip_offset))
                dst[j] = color_for_format(dst_format, dst[j]).blend(color).value();
        dst += dst_skip;
    }
}

// The callback will only be called for a quarter of the ellipse, the user is intended to deduce other points.
// As the coordinate space is relative to the center of the rectangle, it's simply (x, y), (x, -y), (-x, y) and (-x, -y).
static void on_each_ellipse_point(IntRect const& rect, Function<void(IntPoint)>&& callback)
{
    // Note: This is an implementation of the Midpoint Ellipse Algorithm.
    double const a = rect.width() / 2;
    double const a_square = a * a;
    double const b = rect.height() / 2;
    double const b_square = b * b;

    int x = 0;
    auto y = static_cast<int>(b);

    double dx = 2 * b_square * x;
    double dy = 2 * a_square * y;

    // For region 1:
    auto decision_parameter = b_square - a_square * b + .25 * a_square;

    while (dx < dy) {
        callback({ x, y });

        if (decision_parameter >= 0) {
            y--;
            dy -= 2 * a_square;
            decision_parameter -= dy;
        }
        x++;
        dx += 2 * b_square;
        decision_parameter += dx + b_square;
    }

    // For region 2:
    decision_parameter = b_square * ((x + 0.5) * (x + 0.5)) + a_square * ((y - 1) * (y - 1)) - a_square * b_square;

    while (y >= 0) {
        callback({ x, y });

        if (decision_parameter <= 0) {
            x++;
            dx += 2 * b_square;
            decision_parameter += dx;
        }
        y--;
        dy -= 2 * a_square;
        decision_parameter += a_square - dy;
    }
}

void DeprecatedPainter::fill_ellipse(IntRect const& a_rect, Color color)
{
    auto rect = a_rect.translated(translation()).intersected(clip_rect());
    if (rect.is_empty())
        return;

    VERIFY(target().rect().contains(rect));

    auto const center = a_rect.center();

    on_each_ellipse_point(rect, [this, &color, center](IntPoint position) {
        IntPoint const directions[4] = { { position.x(), position.y() }, { -position.x(), position.y() }, { position.x(), -position.y() }, { -position.x(), -position.y() } };

        draw_line(center + directions[0], center + directions[1], color);
        draw_line(center + directions[2], center + directions[3], color);
    });
}

template<typename RectType, typename Callback>
static void for_each_pixel_around_rect_clockwise(RectType const& rect, Callback callback)
{
    if (rect.is_empty())
        return;
    for (auto x = rect.left(); x < rect.right(); ++x)
        callback(x, rect.top());
    for (auto y = rect.top() + 1; y < rect.bottom(); ++y)
        callback(rect.right() - 1, y);
    for (auto x = rect.right() - 2; x >= rect.left(); --x)
        callback(x, rect.bottom() - 1);
    for (auto y = rect.bottom() - 2; y > rect.top(); --y)
        callback(rect.left(), y);
}

void DeprecatedPainter::draw_rect(IntRect const& a_rect, Color color, bool rough)
{
    IntRect rect = a_rect.translated(translation());
    auto clipped_rect = rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;

    int min_y = clipped_rect.top();
    int max_y = clipped_rect.bottom() - 1;

    if (rect.top() >= clipped_rect.top() && rect.top() < clipped_rect.bottom()) {
        int width = rough ? max(0, min(rect.width() - 2, clipped_rect.width())) : clipped_rect.width();
        if (width > 0) {
            int start_x = rough ? max(rect.x() + 1, clipped_rect.x()) : clipped_rect.x();
            fill_physical_scanline(rect.top(), start_x, width, color);
        }
        ++min_y;
    }
    if (rect.bottom() > clipped_rect.top() && rect.bottom() <= clipped_rect.bottom()) {
        int width = rough ? max(0, min(rect.width() - 2, clipped_rect.width())) : clipped_rect.width();
        if (width > 0) {
            int start_x = rough ? max(rect.x() + 1, clipped_rect.x()) : clipped_rect.x();
            fill_physical_scanline(max_y, start_x, width, color);
        }
        --max_y;
    }

    bool draw_left_side = rect.left() >= clipped_rect.left();
    bool draw_right_side = rect.right() == clipped_rect.right();

    if (draw_left_side && draw_right_side) {
        // Specialized loop when drawing both sides.
        for (int y = min_y; y <= max_y; ++y) {
            auto* bits = target().scanline(y);
            set_physical_pixel(bits[rect.left()], color);
            set_physical_pixel(bits[(rect.right() - 1)], color);
        }
    } else {
        for (int y = min_y; y <= max_y; ++y) {
            auto* bits = target().scanline(y);
            if (draw_left_side)
                set_physical_pixel(bits[rect.left()], color);
            if (draw_right_side)
                set_physical_pixel(bits[(rect.right() - 1)], color);
        }
    }
}

struct BlitState {
    enum AlphaState {
        NoAlpha = 0,
        SrcAlpha = 1,
        DstAlpha = 2,
        BothAlpha = SrcAlpha | DstAlpha
    };

    ARGB32 const* src;
    ARGB32* dst;
    size_t src_pitch;
    size_t dst_pitch;
    int row_count;
    int column_count;
    float opacity;
    BitmapFormat src_format;
};

// FIXME: This is a hack to support blit_with_opacity() with RGBA8888 source.
//        Ideally we'd have a more generic solution that allows any source format.
static void swap_red_and_blue_channels(Color& color)
{
    u32 rgba = color.value();
    u32 bgra = (rgba & 0xff00ff00)
        | ((rgba & 0x000000ff) << 16)
        | ((rgba & 0x00ff0000) >> 16);
    color = Color::from_argb(bgra);
}

// FIXME: This function is very unoptimized.
template<BlitState::AlphaState has_alpha>
static void do_blit_with_opacity(BlitState& state)
{
    for (int row = 0; row < state.row_count; ++row) {
        for (int x = 0; x < state.column_count; ++x) {
            Color dest_color = (has_alpha & BlitState::DstAlpha) ? Color::from_argb(state.dst[x]) : Color::from_rgb(state.dst[x]);
            if constexpr (has_alpha & BlitState::SrcAlpha) {
                Color src_color_with_alpha = Color::from_argb(state.src[x]);
                if (state.src_format == BitmapFormat::RGBA8888)
                    swap_red_and_blue_channels(src_color_with_alpha);
                float pixel_opacity = src_color_with_alpha.alpha() / 255.0;
                src_color_with_alpha.set_alpha(255 * (state.opacity * pixel_opacity));
                state.dst[x] = dest_color.blend(src_color_with_alpha).value();
            } else {
                Color src_color_with_alpha = Color::from_rgb(state.src[x]);
                if (state.src_format == BitmapFormat::RGBA8888)
                    swap_red_and_blue_channels(src_color_with_alpha);
                src_color_with_alpha.set_alpha(state.opacity * 255);
                state.dst[x] = dest_color.blend(src_color_with_alpha).value();
            }
        }
        state.dst += state.dst_pitch;
        state.src += state.src_pitch;
    }
}

void DeprecatedPainter::blit_with_opacity(IntPoint position, Gfx::Bitmap const& source, IntRect const& src_rect, float opacity, bool apply_alpha)
{
    if (opacity >= 1.0f && !(source.has_alpha_channel() && apply_alpha))
        return blit(position, source, src_rect);

    IntRect safe_src_rect = IntRect::intersection(src_rect, source.rect());

    auto dst_rect = IntRect(position, safe_src_rect.size()).translated(translation());
    auto clipped_rect = dst_rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;

    int const first_row = clipped_rect.top() - dst_rect.top();
    int const last_row = clipped_rect.bottom() - dst_rect.top();
    int const first_column = clipped_rect.left() - dst_rect.left();
    int const last_column = clipped_rect.right() - dst_rect.left();

    BlitState blit_state {
        .src = source.scanline(src_rect.top() + first_row) + src_rect.left() + first_column,
        .dst = target().scanline(clipped_rect.y()) + clipped_rect.x(),
        .src_pitch = source.pitch() / sizeof(ARGB32),
        .dst_pitch = target().pitch() / sizeof(ARGB32),
        .row_count = last_row - first_row,
        .column_count = last_column - first_column,
        .opacity = opacity,
        .src_format = source.format(),
    };

    if (source.has_alpha_channel() && apply_alpha) {
        if (target().has_alpha_channel())
            do_blit_with_opacity<BlitState::BothAlpha>(blit_state);
        else
            do_blit_with_opacity<BlitState::SrcAlpha>(blit_state);
    } else {
        if (target().has_alpha_channel())
            do_blit_with_opacity<BlitState::DstAlpha>(blit_state);
        else
            do_blit_with_opacity<BlitState::NoAlpha>(blit_state);
    }
}

void DeprecatedPainter::blit_filtered(IntPoint position, Gfx::Bitmap const& source, IntRect const& src_rect, Function<Color(Color)> const& filter, bool apply_alpha)
{
    IntRect safe_src_rect = src_rect.intersected(source.rect());
    auto dst_rect = IntRect(position, safe_src_rect.size()).translated(translation());
    auto clipped_rect = dst_rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;

    int const first_row = clipped_rect.top() - dst_rect.top();
    int const last_row = clipped_rect.bottom() - dst_rect.top();
    int const first_column = clipped_rect.left() - dst_rect.left();
    int const last_column = clipped_rect.right() - dst_rect.left();
    ARGB32* dst = target().scanline(clipped_rect.y()) + clipped_rect.x();
    size_t const dst_skip = target().pitch() / sizeof(ARGB32);
    auto dst_format = target().format();
    auto src_format = source.format();

    ARGB32 const* src = source.scanline(safe_src_rect.top() + first_row) + safe_src_rect.left() + first_column;
    size_t const src_skip = source.pitch() / sizeof(ARGB32);

    for (int row = first_row; row < last_row; ++row) {
        for (int x = 0; x < (last_column - first_column); ++x) {
            auto source_color = color_for_format(src_format, src[x]);
            if (source_color.alpha() == 0)
                continue;
            auto filtered_color = filter(source_color);
            if (!apply_alpha || filtered_color.alpha() == 0xff)
                dst[x] = filtered_color.value();
            else
                dst[x] = color_for_format(dst_format, dst[x]).blend(filtered_color).value();
        }
        dst += dst_skip;
        src += src_skip;
    }
}

void DeprecatedPainter::blit(IntPoint position, Gfx::Bitmap const& source, IntRect const& src_rect, float opacity, bool apply_alpha)
{
    if (opacity < 1.0f || (source.has_alpha_channel() && apply_alpha))
        return blit_with_opacity(position, source, src_rect, opacity, apply_alpha);

    auto safe_src_rect = src_rect.intersected(source.rect());

    // If we get here, the DeprecatedPainter might have a scale factor, but the source bitmap has the same scale factor.
    // We need to transform from logical to physical coordinates, but we can just copy pixels without resampling.
    auto dst_rect = IntRect(position, safe_src_rect.size()).translated(translation());
    auto clipped_rect = dst_rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;

    int const first_row = clipped_rect.top() - dst_rect.top();
    int const last_row = clipped_rect.bottom() - dst_rect.top();
    int const first_column = clipped_rect.left() - dst_rect.left();
    ARGB32* dst = target().scanline(clipped_rect.y()) + clipped_rect.x();
    size_t const dst_skip = target().pitch() / sizeof(ARGB32);

    if (source.format() == BitmapFormat::BGRx8888 || source.format() == BitmapFormat::BGRA8888) {
        ARGB32 const* src = source.scanline(src_rect.top() + first_row) + src_rect.left() + first_column;
        size_t const src_skip = source.pitch() / sizeof(ARGB32);
        for (int row = first_row; row < last_row; ++row) {
            memcpy(dst, src, sizeof(ARGB32) * clipped_rect.width());
            dst += dst_skip;
            src += src_skip;
        }
        return;
    }

    if (source.format() == BitmapFormat::RGBA8888) {
        u32 const* src = source.scanline(src_rect.top() + first_row) + src_rect.left() + first_column;
        size_t const src_skip = source.pitch() / sizeof(u32);
        for (int row = first_row; row < last_row; ++row) {
            for (int i = 0; i < clipped_rect.width(); ++i) {
                u32 rgba = src[i];
                u32 bgra = (rgba & 0xff00ff00)
                    | ((rgba & 0x000000ff) << 16)
                    | ((rgba & 0x00ff0000) >> 16);
                dst[i] = bgra;
            }
            dst += dst_skip;
            src += src_skip;
        }
        return;
    }

    VERIFY_NOT_REACHED();
}

template<bool has_alpha_channel, typename GetPixel>
ALWAYS_INLINE static void do_draw_integer_scaled_bitmap(Gfx::Bitmap& target, IntRect const& dst_rect, IntRect const& src_rect, Gfx::Bitmap const& source, int hfactor, int vfactor, GetPixel get_pixel, float opacity)
{
    bool has_opacity = opacity != 1.0f;
    for (int y = 0; y < src_rect.height(); ++y) {
        int dst_y = dst_rect.y() + y * vfactor;
        for (int x = 0; x < src_rect.width(); ++x) {
            auto src_pixel = get_pixel(source, x + src_rect.left(), y + src_rect.top());
            if (has_opacity)
                src_pixel.set_alpha(src_pixel.alpha() * opacity);
            for (int yo = 0; yo < vfactor; ++yo) {
                auto* scanline = (Color*)target.scanline(dst_y + yo);
                int dst_x = dst_rect.x() + x * hfactor;
                for (int xo = 0; xo < hfactor; ++xo) {
                    if constexpr (has_alpha_channel)
                        scanline[dst_x + xo] = scanline[dst_x + xo].blend(src_pixel);
                    else
                        scanline[dst_x + xo] = src_pixel;
                }
            }
        }
    }
}

template<bool has_alpha_channel, typename GetPixel>
ALWAYS_INLINE static void do_draw_box_sampled_scaled_bitmap(Gfx::Bitmap& target, IntRect const& dst_rect, IntRect const& clipped_rect, Gfx::Bitmap const& source, FloatRect const& src_rect, GetPixel get_pixel, float opacity)
{
    float source_pixel_width = src_rect.width() / dst_rect.width();
    float source_pixel_height = src_rect.height() / dst_rect.height();
    float source_pixel_area = source_pixel_width * source_pixel_height;
    FloatRect const pixel_box = { 0.f, 0.f, 1.f, 1.f };

    for (int y = clipped_rect.top(); y < clipped_rect.bottom(); ++y) {
        auto* scanline = reinterpret_cast<Color*>(target.scanline(y));
        for (int x = clipped_rect.left(); x < clipped_rect.right(); ++x) {
            // Project the destination pixel in the source image
            FloatRect const source_box = {
                src_rect.left() + (x - dst_rect.x()) * source_pixel_width,
                src_rect.top() + (y - dst_rect.y()) * source_pixel_height,
                source_pixel_width,
                source_pixel_height,
            };
            IntRect enclosing_source_box = enclosing_int_rect(source_box).intersected(source.rect());

            // Sum the contribution of all source pixels inside the projected pixel
            float red_accumulator = 0.f;
            float green_accumulator = 0.f;
            float blue_accumulator = 0.f;
            float total_area = 0.f;
            for (int sy = enclosing_source_box.y(); sy < enclosing_source_box.bottom(); ++sy) {
                for (int sx = enclosing_source_box.x(); sx < enclosing_source_box.right(); ++sx) {
                    float area = source_box.intersected(pixel_box.translated(sx, sy)).size().area();

                    auto pixel = get_pixel(source, sx, sy);
                    area *= pixel.alpha() / 255.f;

                    red_accumulator += pixel.red() * area;
                    green_accumulator += pixel.green() * area;
                    blue_accumulator += pixel.blue() * area;
                    total_area += area;
                }
            }

            Color src_pixel = {
                round_to<u8>(min(red_accumulator / total_area, 255.f)),
                round_to<u8>(min(green_accumulator / total_area, 255.f)),
                round_to<u8>(min(blue_accumulator / total_area, 255.f)),
                round_to<u8>(min(total_area * 255.f / source_pixel_area * opacity, 255.f)),
            };

            if constexpr (has_alpha_channel)
                scanline[x] = scanline[x].blend(src_pixel);
            else
                scanline[x] = src_pixel;
        }
    }
}

template<bool has_alpha_channel, ScalingMode scaling_mode, typename GetPixel>
ALWAYS_INLINE static void do_draw_scaled_bitmap(Gfx::Bitmap& target, IntRect const& dst_rect, IntRect const& clipped_rect, Gfx::Bitmap const& source, FloatRect const& src_rect, GetPixel get_pixel, float opacity)
{
    auto int_src_rect = enclosing_int_rect(src_rect);
    auto clipped_src_rect = int_src_rect.intersected(source.rect());
    if (clipped_src_rect.is_empty())
        return;

    if constexpr (scaling_mode == ScalingMode::NearestNeighbor || scaling_mode == ScalingMode::SmoothPixels) {
        if (dst_rect == clipped_rect && int_src_rect == src_rect && !(dst_rect.width() % int_src_rect.width()) && !(dst_rect.height() % int_src_rect.height())) {
            int hfactor = dst_rect.width() / int_src_rect.width();
            int vfactor = dst_rect.height() / int_src_rect.height();
            if (hfactor == 2 && vfactor == 2)
                return do_draw_integer_scaled_bitmap<has_alpha_channel>(target, dst_rect, int_src_rect, source, 2, 2, get_pixel, opacity);
            if (hfactor == 3 && vfactor == 3)
                return do_draw_integer_scaled_bitmap<has_alpha_channel>(target, dst_rect, int_src_rect, source, 3, 3, get_pixel, opacity);
            if (hfactor == 4 && vfactor == 4)
                return do_draw_integer_scaled_bitmap<has_alpha_channel>(target, dst_rect, int_src_rect, source, 4, 4, get_pixel, opacity);
            return do_draw_integer_scaled_bitmap<has_alpha_channel>(target, dst_rect, int_src_rect, source, hfactor, vfactor, get_pixel, opacity);
        }
    }

    if constexpr (scaling_mode == ScalingMode::BoxSampling)
        return do_draw_box_sampled_scaled_bitmap<has_alpha_channel>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);

    bool has_opacity = opacity != 1.f;
    i64 shift = 1ll << 32;
    i64 fractional_mask = shift - 1;
    i64 bilinear_offset_x = (1ll << 31) * (src_rect.width() / dst_rect.width() - 1);
    i64 bilinear_offset_y = (1ll << 31) * (src_rect.height() / dst_rect.height() - 1);
    i64 hscale = src_rect.width() * shift / dst_rect.width();
    i64 vscale = src_rect.height() * shift / dst_rect.height();
    i64 src_left = src_rect.left() * shift;
    i64 src_top = src_rect.top() * shift;

    for (int y = clipped_rect.top(); y < clipped_rect.bottom(); ++y) {
        auto* scanline = reinterpret_cast<Color*>(target.scanline(y));
        auto desired_y = (y - dst_rect.y()) * vscale + src_top;

        for (int x = clipped_rect.left(); x < clipped_rect.right(); ++x) {
            auto desired_x = (x - dst_rect.x()) * hscale + src_left;

            Color src_pixel;
            if constexpr (scaling_mode == ScalingMode::BilinearBlend) {
                auto shifted_x = desired_x + bilinear_offset_x;
                auto shifted_y = desired_y + bilinear_offset_y;

                auto scaled_x0 = clamp(shifted_x >> 32, clipped_src_rect.left(), clipped_src_rect.right() - 1);
                auto scaled_x1 = clamp((shifted_x >> 32) + 1, clipped_src_rect.left(), clipped_src_rect.right() - 1);
                auto scaled_y0 = clamp(shifted_y >> 32, clipped_src_rect.top(), clipped_src_rect.bottom() - 1);
                auto scaled_y1 = clamp((shifted_y >> 32) + 1, clipped_src_rect.top(), clipped_src_rect.bottom() - 1);

                float x_ratio = (shifted_x & fractional_mask) / static_cast<float>(shift);
                float y_ratio = (shifted_y & fractional_mask) / static_cast<float>(shift);

                auto top_left = get_pixel(source, scaled_x0, scaled_y0);
                auto top_right = get_pixel(source, scaled_x1, scaled_y0);
                auto bottom_left = get_pixel(source, scaled_x0, scaled_y1);
                auto bottom_right = get_pixel(source, scaled_x1, scaled_y1);

                auto top = top_left.mixed_with(top_right, x_ratio);
                auto bottom = bottom_left.mixed_with(bottom_right, x_ratio);

                src_pixel = top.mixed_with(bottom, y_ratio);
            } else if constexpr (scaling_mode == ScalingMode::SmoothPixels) {
                auto scaled_x1 = clamp(desired_x >> 32, clipped_src_rect.left(), clipped_src_rect.right() - 1);
                auto scaled_x0 = clamp(scaled_x1 - 1, clipped_src_rect.left(), clipped_src_rect.right() - 1);
                auto scaled_y1 = clamp(desired_y >> 32, clipped_src_rect.top(), clipped_src_rect.bottom() - 1);
                auto scaled_y0 = clamp(scaled_y1 - 1, clipped_src_rect.top(), clipped_src_rect.bottom() - 1);

                float x_ratio = (desired_x & fractional_mask) / (float)shift;
                float y_ratio = (desired_y & fractional_mask) / (float)shift;

                float scaled_x_ratio = clamp(x_ratio * dst_rect.width() / (float)src_rect.width(), 0.f, 1.f);
                float scaled_y_ratio = clamp(y_ratio * dst_rect.height() / (float)src_rect.height(), 0.f, 1.f);

                auto top_left = get_pixel(source, scaled_x0, scaled_y0);
                auto top_right = get_pixel(source, scaled_x1, scaled_y0);
                auto bottom_left = get_pixel(source, scaled_x0, scaled_y1);
                auto bottom_right = get_pixel(source, scaled_x1, scaled_y1);

                auto top = top_left.mixed_with(top_right, scaled_x_ratio);
                auto bottom = bottom_left.mixed_with(bottom_right, scaled_x_ratio);

                src_pixel = top.mixed_with(bottom, scaled_y_ratio);
            } else {
                auto scaled_x = clamp(desired_x >> 32, clipped_src_rect.left(), clipped_src_rect.right() - 1);
                auto scaled_y = clamp(desired_y >> 32, clipped_src_rect.top(), clipped_src_rect.bottom() - 1);
                src_pixel = get_pixel(source, scaled_x, scaled_y);
            }

            if (has_opacity)
                src_pixel.set_alpha(src_pixel.alpha() * opacity);

            if constexpr (has_alpha_channel)
                scanline[x] = scanline[x].blend(src_pixel);
            else
                scanline[x] = src_pixel;
        }
    }
}

template<bool has_alpha_channel, typename GetPixel>
ALWAYS_INLINE static void do_draw_scaled_bitmap(Gfx::Bitmap& target, IntRect const& dst_rect, IntRect const& clipped_rect, Gfx::Bitmap const& source, FloatRect const& src_rect, GetPixel get_pixel, float opacity, ScalingMode scaling_mode)
{
    switch (scaling_mode) {
    case ScalingMode::NearestNeighbor:
        do_draw_scaled_bitmap<has_alpha_channel, ScalingMode::NearestNeighbor>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);
        break;
    case ScalingMode::SmoothPixels:
        do_draw_scaled_bitmap<has_alpha_channel, ScalingMode::SmoothPixels>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);
        break;
    case ScalingMode::BilinearBlend:
        do_draw_scaled_bitmap<has_alpha_channel, ScalingMode::BilinearBlend>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);
        break;
    case ScalingMode::BoxSampling:
        do_draw_scaled_bitmap<has_alpha_channel, ScalingMode::BoxSampling>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);
        break;
    case ScalingMode::None:
        do_draw_scaled_bitmap<has_alpha_channel, ScalingMode::None>(target, dst_rect, clipped_rect, source, src_rect, get_pixel, opacity);
        break;
    }
}

void DeprecatedPainter::draw_scaled_bitmap(IntRect const& a_dst_rect, Gfx::Bitmap const& source, IntRect const& a_src_rect, float opacity, ScalingMode scaling_mode)
{
    draw_scaled_bitmap(a_dst_rect, source, FloatRect { a_src_rect }, opacity, scaling_mode);
}

void DeprecatedPainter::draw_scaled_bitmap(IntRect const& a_dst_rect, Gfx::Bitmap const& source, FloatRect const& a_src_rect, float opacity, ScalingMode scaling_mode)
{
    IntRect int_src_rect = enclosing_int_rect(a_src_rect);
    if (a_src_rect == int_src_rect && a_dst_rect.size() == int_src_rect.size())
        return blit(a_dst_rect.location(), source, int_src_rect, opacity);

    if (scaling_mode == ScalingMode::None) {
        IntRect clipped_draw_rect { (int)a_src_rect.location().x(), (int)a_src_rect.location().y(), a_dst_rect.size().width(), a_dst_rect.size().height() };
        return blit(a_dst_rect.location(), source, clipped_draw_rect, opacity);
    }

    auto dst_rect = to_physical(a_dst_rect);
    auto src_rect = a_src_rect;
    auto clipped_rect = dst_rect.intersected(clip_rect());
    if (clipped_rect.is_empty())
        return;

    if (source.has_alpha_channel() || opacity != 1.0f) {
        switch (source.format()) {
        case BitmapFormat::BGRx8888:
            do_draw_scaled_bitmap<true>(*m_target, dst_rect, clipped_rect, source, src_rect, Gfx::get_pixel<BitmapFormat::BGRx8888>, opacity, scaling_mode);
            break;
        case BitmapFormat::BGRA8888:
            do_draw_scaled_bitmap<true>(*m_target, dst_rect, clipped_rect, source, src_rect, Gfx::get_pixel<BitmapFormat::BGRA8888>, opacity, scaling_mode);
            break;
        default:
            do_draw_scaled_bitmap<true>(*m_target, dst_rect, clipped_rect, source, src_rect, Gfx::get_pixel<BitmapFormat::Invalid>, opacity, scaling_mode);
            break;
        }
    } else {
        switch (source.format()) {
        case BitmapFormat::BGRx8888:
            do_draw_scaled_bitmap<false>(*m_target, dst_rect, clipped_rect, source, src_rect, Gfx::get_pixel<BitmapFormat::BGRx8888>, opacity, scaling_mode);
            break;
        default:
            do_draw_scaled_bitmap<false>(*m_target, dst_rect, clipped_rect, source, src_rect, Gfx::get_pixel<BitmapFormat::Invalid>, opacity, scaling_mode);
            break;
        }
    }
}

void DeprecatedPainter::set_pixel(IntPoint p, Color color, bool blend)
{
    auto point = p;
    point.translate_by(state().translation);
    // Use the scale only to avoid clipping pixels set in drawing functions that handle
    // scaling and call set_pixel() -- do not scale the pixel.
    if (!clip_rect().contains(point))
        return;
    set_physical_pixel(point, color, blend);
}

void DeprecatedPainter::set_physical_pixel(IntPoint physical_point, Color color, bool blend)
{
    // This function should only be called after translation, clipping, etc has been handled elsewhere
    // if not use set_pixel().
    auto& dst = target().scanline(physical_point.y())[physical_point.x()];
    if (!blend || color.alpha() == 255)
        dst = color.value();
    else if (color.alpha())
        dst = color_for_format(target().format(), dst).blend(color).value();
}

Optional<Color> DeprecatedPainter::get_pixel(IntPoint p)
{
    auto point = p;
    point.translate_by(state().translation);
    if (!clip_rect().contains(point))
        return {};
    return target().get_pixel(point);
}

ErrorOr<NonnullRefPtr<Bitmap>> DeprecatedPainter::get_region_bitmap(IntRect const& region, BitmapFormat format, Optional<IntRect&> actual_region)
{
    auto bitmap_region = region.translated(state().translation).intersected(target().rect());
    if (actual_region.has_value())
        actual_region.value() = bitmap_region.translated(-state().translation);
    return target().cropped(bitmap_region, format);
}

ALWAYS_INLINE void DeprecatedPainter::set_physical_pixel(u32& pixel, Color color)
{
    // This always sets a single physical pixel, independent of scale().
    // This should only be called by routines that already handle scale.
    pixel = color.value();
}

ALWAYS_INLINE void DeprecatedPainter::fill_physical_scanline(int y, int x, int width, Color color)
{
    // This always draws a single physical scanline, independent of scale().
    // This should only be called by routines that already handle scale.
    fast_u32_fill(target().scanline(y) + x, color.value(), width);
}

void DeprecatedPainter::draw_physical_pixel(IntPoint physical_position, Color color, int thickness)
{
    // This always draws a single physical pixel, independent of scale().
    // This should only be called by routines that already handle scale
    // (including scaling thickness).
    if (thickness <= 0)
        return;

    if (thickness == 1) { // Implies scale() == 1.
        auto& pixel = target().scanline(physical_position.y())[physical_position.x()];
        return set_physical_pixel(pixel, color_for_format(target().format(), pixel).blend(color));
    }

    IntRect rect { physical_position, { thickness, thickness } };
    rect.intersect(clip_rect());
    fill_physical_rect(rect, color);
}

void DeprecatedPainter::draw_line(IntPoint a_p1, IntPoint a_p2, Color color, int thickness, LineStyle style, Color alternate_color)
{
    if (clip_rect().is_empty())
        return;

    if (thickness <= 0)
        return;

    if (color.alpha() == 0)
        return;

    auto clip_rect = this->clip_rect();

    auto const p1 = thickness > 1 ? a_p1.translated(-(thickness / 2), -(thickness / 2)) : a_p1;
    auto const p2 = thickness > 1 ? a_p2.translated(-(thickness / 2), -(thickness / 2)) : a_p2;

    auto point1 = to_physical(p1);
    auto point2 = to_physical(p2);

    auto alternate_color_is_transparent = alternate_color == Color::Transparent;

    // Special case: vertical line.
    if (point1.x() == point2.x()) {
        int const x = point1.x();
        if (x < clip_rect.left() || x >= clip_rect.right())
            return;
        if (point1.y() > point2.y())
            swap(point1, point2);
        if (point1.y() >= clip_rect.bottom())
            return;
        if (point2.y() < clip_rect.top())
            return;
        int min_y = max(point1.y(), clip_rect.top());
        int max_y = min(point2.y(), clip_rect.bottom() - 1);
        if (style == LineStyle::Dotted) {
            for (int y = min_y; y <= max_y; y += thickness * 2)
                draw_physical_pixel({ x, y }, color, thickness);
        } else if (style == LineStyle::Dashed) {
            for (int y = min_y; y <= max_y; y += thickness * 6) {
                draw_physical_pixel({ x, y }, color, thickness);
                draw_physical_pixel({ x, min(y + thickness, max_y) }, color, thickness);
                draw_physical_pixel({ x, min(y + thickness * 2, max_y) }, color, thickness);
                if (!alternate_color_is_transparent) {
                    draw_physical_pixel({ x, min(y + thickness * 3, max_y) }, alternate_color, thickness);
                    draw_physical_pixel({ x, min(y + thickness * 4, max_y) }, alternate_color, thickness);
                    draw_physical_pixel({ x, min(y + thickness * 5, max_y) }, alternate_color, thickness);
                }
            }
        } else {
            for (int y = min_y; y <= max_y; y += thickness)
                draw_physical_pixel({ x, y }, color, thickness);
            draw_physical_pixel({ x, max_y }, color, thickness);
        }
        return;
    }

    // Special case: horizontal line.
    if (point1.y() == point2.y()) {
        int const y = point1.y();
        if (y < clip_rect.top() || y >= clip_rect.bottom())
            return;
        if (point1.x() > point2.x())
            swap(point1, point2);
        if (point1.x() >= clip_rect.right())
            return;
        if (point2.x() < clip_rect.left())
            return;
        int min_x = max(point1.x(), clip_rect.left());
        int max_x = min(point2.x(), clip_rect.right() - 1);
        if (style == LineStyle::Dotted) {
            for (int x = min_x; x <= max_x; x += thickness * 2)
                draw_physical_pixel({ x, y }, color, thickness);
        } else if (style == LineStyle::Dashed) {
            for (int x = min_x; x <= max_x; x += thickness * 6) {
                draw_physical_pixel({ x, y }, color, thickness);
                draw_physical_pixel({ min(x + thickness, max_x), y }, color, thickness);
                draw_physical_pixel({ min(x + thickness * 2, max_x), y }, color, thickness);
                if (!alternate_color_is_transparent) {
                    draw_physical_pixel({ min(x + thickness * 3, max_x), y }, alternate_color, thickness);
                    draw_physical_pixel({ min(x + thickness * 4, max_x), y }, alternate_color, thickness);
                    draw_physical_pixel({ min(x + thickness * 5, max_x), y }, alternate_color, thickness);
                }
            }
        } else {
            for (int x = min_x; x <= max_x; x += thickness)
                draw_physical_pixel({ x, y }, color, thickness);
            draw_physical_pixel({ max_x, y }, color, thickness);
        }
        return;
    }

    int const adx = abs(point2.x() - point1.x());
    int const ady = abs(point2.y() - point1.y());

    if (adx > ady) {
        if (point1.x() > point2.x())
            swap(point1, point2);
    } else {
        if (point1.y() > point2.y())
            swap(point1, point2);
    }

    int const dx = point2.x() - point1.x();
    int const dy = point2.y() - point1.y();
    int error = 0;

    size_t number_of_pixels_drawn = 0;

    auto draw_pixel_in_line = [&](int x, int y) {
        bool should_draw_line = true;
        if (style == LineStyle::Dotted && number_of_pixels_drawn % 2 == 1)
            should_draw_line = false;
        else if (style == LineStyle::Dashed && number_of_pixels_drawn % 6 >= 3)
            should_draw_line = false;

        if (should_draw_line)
            draw_physical_pixel({ x, y }, color, thickness);
        else if (!alternate_color_is_transparent)
            draw_physical_pixel({ x, y }, alternate_color, thickness);

        number_of_pixels_drawn++;
    };

    if (dx > dy) {
        int const y_step = dy == 0 ? 0 : (dy > 0 ? 1 : -1);
        int const delta_error = 2 * abs(dy);
        int y = point1.y();
        for (int x = point1.x(); x <= point2.x(); ++x) {
            if (clip_rect.contains(x, y))
                draw_pixel_in_line(x, y);
            error += delta_error;
            if (error >= dx) {
                y += y_step;
                error -= 2 * dx;
            }
        }
    } else {
        int const x_step = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
        int const delta_error = 2 * abs(dx);
        int x = point1.x();
        for (int y = point1.y(); y <= point2.y(); ++y) {
            if (clip_rect.contains(x, y))
                draw_pixel_in_line(x, y);
            error += delta_error;
            if (error >= dy) {
                x += x_step;
                error -= 2 * dy;
            }
        }
    }
}

void DeprecatedPainter::draw_triangle_wave(IntPoint a_p1, IntPoint a_p2, Color color, int amplitude, int thickness)
{
    // FIXME: Support more than horizontal waves
    VERIFY(a_p1.y() == a_p2.y());

    auto const p1 = thickness > 1 ? a_p1.translated(-(thickness / 2), -(thickness / 2)) : a_p1;
    auto const p2 = thickness > 1 ? a_p2.translated(-(thickness / 2), -(thickness / 2)) : a_p2;

    auto point1 = to_physical(p1);
    auto point2 = to_physical(p2);

    auto y = point1.y();

    for (int x = 0; x <= point2.x() - point1.x(); ++x) {
        auto y_offset = abs(x % (2 * amplitude) - amplitude) - amplitude;
        draw_physical_pixel({ point1.x() + x, y + y_offset }, color, thickness);
    }
}

static bool can_approximate_bezier_curve(FloatPoint p1, FloatPoint p2, FloatPoint control)
{
    // TODO: Somehow calculate the required number of splits based on the curve (and its size).
    constexpr float tolerance = 0.5f;

    auto p1x = 3 * control.x() - 2 * p1.x() - p2.x();
    auto p1y = 3 * control.y() - 2 * p1.y() - p2.y();
    auto p2x = 3 * control.x() - 2 * p2.x() - p1.x();
    auto p2y = 3 * control.y() - 2 * p2.y() - p1.y();

    p1x = p1x * p1x;
    p1y = p1y * p1y;
    p2x = p2x * p2x;
    p2y = p2y * p2y;

    auto error = max(p1x, p2x) + max(p1y, p2y);
    VERIFY(isfinite(error));

    return error <= tolerance;
}

void DeprecatedPainter::for_each_line_segment_on_bezier_curve(FloatPoint control_point, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>& callback)
{
    struct SegmentDescriptor {
        FloatPoint control_point;
        FloatPoint p1;
        FloatPoint p2;
    };

    static constexpr auto split_quadratic_bezier_curve = [](FloatPoint original_control, FloatPoint p1, FloatPoint p2, auto& segments) {
        auto po1_midpoint = original_control + p1;
        po1_midpoint /= 2;

        auto po2_midpoint = original_control + p2;
        po2_midpoint /= 2;

        auto new_segment = po1_midpoint + po2_midpoint;
        new_segment /= 2;

        segments.append({ po2_midpoint, new_segment, p2 });
        segments.append({ po1_midpoint, p1, new_segment });
    };

    Vector<SegmentDescriptor> segments;
    segments.append({ control_point, p1, p2 });
    while (!segments.is_empty()) {
        auto segment = segments.take_last();

        if (can_approximate_bezier_curve(segment.p1, segment.p2, segment.control_point))
            callback(segment.p1, segment.p2);
        else
            split_quadratic_bezier_curve(segment.control_point, segment.p1, segment.p2, segments);
    }
}

void DeprecatedPainter::for_each_line_segment_on_bezier_curve(FloatPoint control_point, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&& callback)
{
    for_each_line_segment_on_bezier_curve(control_point, p1, p2, callback);
}

void DeprecatedPainter::for_each_line_segment_on_cubic_bezier_curve(FloatPoint control_point_0, FloatPoint control_point_1, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>&& callback)
{
    for_each_line_segment_on_cubic_bezier_curve(control_point_0, control_point_1, p1, p2, callback);
}

static bool can_approximate_cubic_bezier_curve(FloatPoint p1, FloatPoint p2, FloatPoint control_0, FloatPoint control_1)
{
    // TODO: Somehow calculate the required number of splits based on the curve (and its size).
    constexpr float tolerance = 0.5f;

    auto ax = 3 * control_0.x() - 2 * p1.x() - p2.x();
    auto ay = 3 * control_0.y() - 2 * p1.y() - p2.y();
    auto bx = 3 * control_1.x() - p1.x() - 2 * p2.x();
    auto by = 3 * control_1.y() - p1.y() - 2 * p2.y();

    ax *= ax;
    ay *= ay;
    bx *= bx;
    by *= by;

    auto error = max(ax, bx) + max(ay, by);
    VERIFY(isfinite(error));

    return error <= tolerance;
}

// static
void DeprecatedPainter::for_each_line_segment_on_cubic_bezier_curve(FloatPoint control_point_0, FloatPoint control_point_1, FloatPoint p1, FloatPoint p2, Function<void(FloatPoint, FloatPoint)>& callback)
{
    struct ControlPair {
        FloatPoint control_point_0;
        FloatPoint control_point_1;
    };
    struct SegmentDescriptor {
        ControlPair control_points;
        FloatPoint p1;
        FloatPoint p2;
    };

    static constexpr auto split_cubic_bezier_curve = [](ControlPair const& original_controls, FloatPoint p1, FloatPoint p2, auto& segments) {
        Array level_1_midpoints {
            (p1 + original_controls.control_point_0) / 2,
            (original_controls.control_point_0 + original_controls.control_point_1) / 2,
            (original_controls.control_point_1 + p2) / 2,
        };
        Array level_2_midpoints {
            (level_1_midpoints[0] + level_1_midpoints[1]) / 2,
            (level_1_midpoints[1] + level_1_midpoints[2]) / 2,
        };
        auto level_3_midpoint = (level_2_midpoints[0] + level_2_midpoints[1]) / 2;

        segments.append({ { level_2_midpoints[1], level_1_midpoints[2] }, level_3_midpoint, p2 });
        segments.append({ { level_1_midpoints[0], level_2_midpoints[0] }, p1, level_3_midpoint });
    };

    Vector<SegmentDescriptor> segments;
    segments.append({ { control_point_0, control_point_1 }, p1, p2 });
    while (!segments.is_empty()) {
        auto segment = segments.take_last();

        if (can_approximate_cubic_bezier_curve(segment.p1, segment.p2, segment.control_points.control_point_0, segment.control_points.control_point_1))
            callback(segment.p1, segment.p2);
        else
            split_cubic_bezier_curve(segment.control_points, segment.p1, segment.p2, segments);
    }
}

void DeprecatedPainter::add_clip_rect(IntRect const& rect)
{
    state().clip_rect.intersect(rect.translated(translation()));
    state().clip_rect.intersect(target().rect()); // FIXME: This shouldn't be necessary?
}

void DeprecatedPainter::clear_clip_rect()
{
    state().clip_rect = m_clip_origin;
}

DeprecatedPainterStateSaver::DeprecatedPainterStateSaver(DeprecatedPainter& painter)
    : m_painter(painter)
{
    m_painter.save();
}

DeprecatedPainterStateSaver::~DeprecatedPainterStateSaver()
{
    m_painter.restore();
}

void DeprecatedPainter::stroke_path(DeprecatedPath const& path, Color color, int thickness)
{
    if (thickness <= 0)
        return;
    fill_path(path.stroke_to_fill(thickness), color);
}

void DeprecatedPainter::draw_scaled_bitmap_with_transform(IntRect const& dst_rect, Bitmap const& bitmap, FloatRect const& src_rect, AffineTransform const& transform, float opacity, ScalingMode scaling_mode)
{
    if (transform.is_identity_or_translation_or_scale()) {
        draw_scaled_bitmap(transform.map(dst_rect.to_type<float>()).to_rounded<int>(), bitmap, src_rect, opacity, scaling_mode);
    } else {
        // The painter has an affine transform, we have to draw through it!

        // FIXME: This is kinda inefficient.
        // What we currently do, roughly:
        // - Map the destination rect through the context's transform.
        // - Compute the bounding rect of the destination quad.
        // - For each point in the clipped bounding rect, reverse-map it to a point in the source image.
        //   - Sample the source image at the computed point.
        //   - Set or blend (depending on alpha values) one pixel in the canvas.
        //   - Loop.

        // FIXME: DeprecatedPainter should have an affine transform as part of its state and handle all of this instead.

        if (opacity == 0.0f)
            return;

        auto inverse_transform = transform.inverse();
        if (!inverse_transform.has_value())
            return;

        auto destination_quad = transform.map_to_quad(dst_rect.to_type<float>());
        auto destination_bounding_rect = destination_quad.bounding_rect().to_rounded<int>();
        auto source_rect = enclosing_int_rect(src_rect).intersected(bitmap.rect());

        Gfx::AffineTransform source_transform;
        source_transform.translate(src_rect.x(), src_rect.y());
        source_transform.scale(src_rect.width() / dst_rect.width(), src_rect.height() / dst_rect.height());
        source_transform.translate(-dst_rect.x(), -dst_rect.y());

        auto translated_dest_rect = destination_bounding_rect.translated(translation());
        auto clipped_bounding_rect = translated_dest_rect.intersected(clip_rect());
        if (clipped_bounding_rect.is_empty())
            return;

        auto sample_transform = source_transform.multiply(*inverse_transform);
        auto start_offset = destination_bounding_rect.location() + (clipped_bounding_rect.location() - translated_dest_rect.location());
        for (int y = 0; y < clipped_bounding_rect.height(); ++y) {
            for (int x = 0; x < clipped_bounding_rect.width(); ++x) {
                auto point = Gfx::IntPoint { x, y };
                auto sample_point = point + start_offset;

                // AffineTransform::map(IntPoint) rounds internally, which is wrong here. So explicitly call the FloatPoint version, and then truncate the result.
                auto source_point = Gfx::IntPoint { sample_transform.map(Gfx::FloatPoint { sample_point }) };

                if (!source_rect.contains(source_point))
                    continue;
                auto source_color = bitmap.get_pixel(source_point);
                if (source_color.alpha() == 0)
                    continue;
                if (opacity != 1.0f)
                    source_color = source_color.with_opacity(opacity);
                set_physical_pixel(point + clipped_bounding_rect.location(), source_color, true);
            }
        }
    }
}

}
