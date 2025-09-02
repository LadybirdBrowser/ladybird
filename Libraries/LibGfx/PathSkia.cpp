/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD
#define SK_SUPPORT_UNSPANNED_APIS

#include <AK/TypeCasts.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/TextLayout.h>
#include <core/SkFont.h>
#include <core/SkPath.h>
#include <core/SkPathMeasure.h>
#include <core/SkTextBlob.h>
#include <pathops/SkPathOps.h>
#include <utils/SkTextUtils.h>

namespace Gfx {

NonnullOwnPtr<Gfx::PathImplSkia> PathImplSkia::create()
{
    return adopt_own(*new PathImplSkia);
}

PathImplSkia::PathImplSkia()
    : m_path(adopt_own(*new SkPath))
{
}

PathImplSkia::PathImplSkia(PathImplSkia const& other)
    : m_last_move_to(other.m_last_move_to)
    , m_path(adopt_own(*new SkPath(other.sk_path())))
{
}

PathImplSkia::~PathImplSkia() = default;

void PathImplSkia::clear()
{
    m_path->reset();
}

void PathImplSkia::move_to(Gfx::FloatPoint const& point)
{
    m_last_move_to = point;
    m_path->moveTo(point.x(), point.y());
}

void PathImplSkia::line_to(Gfx::FloatPoint const& point)
{
    m_path->lineTo(point.x(), point.y());
}

void PathImplSkia::close()
{
    m_path->close();
    m_path->moveTo(m_last_move_to.x(), m_last_move_to.y());
}

void PathImplSkia::elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep)
{
    SkPoint skPoint = SkPoint::Make(point.x(), point.y());
    SkScalar skWidth = SkFloatToScalar(radii.width());
    SkScalar skHeight = SkFloatToScalar(radii.height());
    SkScalar skXRotation = SkFloatToScalar(sk_float_radians_to_degrees(x_axis_rotation));
    SkPath::ArcSize skLargeArc = large_arc ? SkPath::kLarge_ArcSize : SkPath::kSmall_ArcSize;
    SkPathDirection skSweep = sweep ? SkPathDirection::kCW : SkPathDirection::kCCW;
    m_path->arcTo(skWidth, skHeight, skXRotation, skLargeArc, skSweep, skPoint.x(), skPoint.y());
}

void PathImplSkia::arc_to(FloatPoint point, float radius, bool large_arc, bool sweep)
{
    SkPoint skPoint = SkPoint::Make(point.x(), point.y());
    SkScalar skRadius = SkFloatToScalar(radius);
    SkPath::ArcSize skLargeArc = large_arc ? SkPath::kLarge_ArcSize : SkPath::kSmall_ArcSize;
    SkPathDirection skSweep = sweep ? SkPathDirection::kCW : SkPathDirection::kCCW;
    m_path->arcTo(skRadius, skRadius, 0, skLargeArc, skSweep, skPoint.x(), skPoint.y());
}

void PathImplSkia::quadratic_bezier_curve_to(FloatPoint through, FloatPoint point)
{
    m_path->quadTo(through.x(), through.y(), point.x(), point.y());
}

void PathImplSkia::cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2)
{
    m_path->cubicTo(c1.x(), c1.y(), c2.x(), c2.y(), p2.x(), p2.y());
}

void PathImplSkia::text(Utf8View const& string, Font const& font)
{
    SkTextUtils::GetPath(string.as_string().characters_without_null_termination(), string.as_string().length(), SkTextEncoding::kUTF8, last_point().x(), last_point().y(), font.skia_font(1), m_path.ptr());
}

void PathImplSkia::text(Utf16View const& string, Font const& font)
{
    if (string.has_ascii_storage()) {
        text(Utf8View { string.bytes() }, font);
        return;
    }

    SkTextUtils::GetPath(string.utf16_span().data(), string.length_in_code_units() * sizeof(char16_t), SkTextEncoding::kUTF16, last_point().x(), last_point().y(), font.skia_font(1), m_path.ptr());
}

void PathImplSkia::glyph_run(GlyphRun const& glyph_run)
{
    auto sk_font = glyph_run.font().skia_font(1);
    m_path->setFillType(SkPathFillType::kWinding);
    auto font_ascent = glyph_run.font().pixel_metrics().ascent;
    for (auto const& glyph : glyph_run.glyphs()) {
        SkPath glyph_path;
        if (!sk_font.getPath(static_cast<SkGlyphID>(glyph.glyph_id), &glyph_path))
            continue;
        glyph_path.offset(glyph.position.x(), glyph.position.y() + font_ascent);
        m_path->addPath(glyph_path);
    }
}

void PathImplSkia::offset(Gfx::FloatPoint const& offset)
{
    m_path->offset(offset.x(), offset.y());
}

template<typename TextToGlyphs>
static NonnullOwnPtr<PathImpl> place_text_along_impl(SkPath const& path, Font const& font, size_t length_in_code_points, TextToGlyphs&& text_to_glyphs)
{
    auto sk_font = font.skia_font(1);
    SkScalar x = 0;
    SkScalar y = 0;

    SkTextBlobBuilder builder;
    auto const& run_buffer = builder.allocRun(sk_font, static_cast<int>(length_in_code_points), x, y, nullptr);
    text_to_glyphs(sk_font, run_buffer);

    SkPathMeasure path_measure(path, false);
    SkScalar accumulated_distance = 0;

    auto output_path = PathImplSkia::create();
    SkScalar path_length = path_measure.getLength();

    for (size_t i = 0; i < length_in_code_points; ++i) {
        SkGlyphID glyph = run_buffer.glyphs[i];
        SkPath glyph_path;
        sk_font.getPath(glyph, &glyph_path);

        SkScalar advance;
        sk_font.getWidths(&glyph, 1, &advance);

        SkPoint position;
        SkVector tangent;
        if (!path_measure.getPosTan(accumulated_distance, &position, &tangent))
            continue;

        // Any typographic characters with mid-points that are not on the path are not rendered.
        SkScalar midpoint_distance = accumulated_distance + (advance / 2.0f);
        if (midpoint_distance > path_length)
            break;

        SkMatrix matrix;
        matrix.setTranslate(position.x(), position.y());
        matrix.preRotate(SkRadiansToDegrees(std::atan2(tangent.y(), tangent.x())));

        glyph_path.transform(matrix);
        output_path->sk_path().addPath(glyph_path);

        accumulated_distance += advance;
    }

    return output_path;
}

NonnullOwnPtr<PathImpl> PathImplSkia::place_text_along(Utf8View const& text, Font const& font) const
{
    auto length_in_code_points = text.length();

    return place_text_along_impl(*m_path, font, length_in_code_points, [&](auto const& sk_font, auto const& run_buffer) {
        sk_font.textToGlyphs(text.as_string().characters_without_null_termination(), text.as_string().length(), SkTextEncoding::kUTF8, run_buffer.glyphs, length_in_code_points);
    });
}

NonnullOwnPtr<PathImpl> PathImplSkia::place_text_along(Utf16View const& text, Font const& font) const
{
    if (text.has_ascii_storage())
        return place_text_along(Utf8View { text.bytes() }, font);

    auto length_in_code_points = text.length_in_code_points();

    return place_text_along_impl(*m_path, font, length_in_code_points, [&](auto const& sk_font, auto const& run_buffer) {
        sk_font.textToGlyphs(text.utf16_span().data(), text.length_in_code_units() * sizeof(char16_t), SkTextEncoding::kUTF16, run_buffer.glyphs, length_in_code_points);
    });
}

void PathImplSkia::append_path(Gfx::Path const& other)
{
    m_path->addPath(static_cast<PathImplSkia const&>(other.impl()).sk_path());
}

void PathImplSkia::intersect(Gfx::Path const& other)
{
    Op(*m_path, static_cast<PathImplSkia const&>(other.impl()).sk_path(), SkPathOp::kIntersect_SkPathOp, m_path.ptr());
}

bool PathImplSkia::is_empty() const
{
    return m_path->isEmpty();
}

Gfx::FloatPoint PathImplSkia::last_point() const
{
    SkPoint last {};
    if (!m_path->getLastPt(&last))
        return {};
    return { last.fX, last.fY };
}

Gfx::FloatRect PathImplSkia::bounding_box() const
{
    auto bounds = m_path->getBounds();
    return { bounds.fLeft, bounds.fTop, bounds.fRight - bounds.fLeft, bounds.fBottom - bounds.fTop };
}

bool PathImplSkia::contains(FloatPoint point, Gfx::WindingRule winding_rule) const
{
    SkPath temp_path = *m_path;
    temp_path.setFillType(to_skia_path_fill_type(winding_rule));
    return temp_path.contains(point.x(), point.y());
}

void PathImplSkia::set_fill_type(Gfx::WindingRule winding_rule)
{
    m_path->setFillType(to_skia_path_fill_type(winding_rule));
}

NonnullOwnPtr<PathImpl> PathImplSkia::clone() const
{
    return adopt_own(*new PathImplSkia(*this));
}

NonnullOwnPtr<PathImpl> PathImplSkia::copy_transformed(Gfx::AffineTransform const& transform) const
{
    auto new_path = adopt_own(*new PathImplSkia(*this));
    auto matrix = SkMatrix::MakeAll(
        transform.a(), transform.c(), transform.e(),
        transform.b(), transform.d(), transform.f(),
        0, 0, 1);
    new_path->sk_path().transform(matrix);
    return new_path;
}

}
