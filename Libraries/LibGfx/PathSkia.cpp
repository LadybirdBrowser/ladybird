/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/TypeCasts.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SkiaUtils.h>
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

void PathImplSkia::close_all_subpaths()
{
    SkPath new_path;
    SkPath::Iter iter(*m_path, false);
    SkPoint points[4];
    SkPath::Verb verb;
    bool need_close = false;

    while ((verb = iter.next(points)) != SkPath::kDone_Verb) {
        switch (verb) {
        case SkPath::kMove_Verb:
            if (need_close) {
                new_path.close();
            }
            new_path.moveTo(points[0]);
            need_close = true;
            break;
        case SkPath::kLine_Verb:
            new_path.lineTo(points[1]);
            break;
        case SkPath::kQuad_Verb:
            new_path.quadTo(points[1], points[2]);
            break;
        case SkPath::kCubic_Verb:
            new_path.cubicTo(points[1], points[2], points[3]);
            break;
        case SkPath::kClose_Verb:
            new_path.close();
            need_close = false;
            break;
        case SkPath::kConic_Verb:
            new_path.conicTo(points[1], points[2], iter.conicWeight());
            break;
        case SkPath::kDone_Verb:
            break;
        }
    }

    if (need_close) {
        new_path.close();
    }

    *m_path = new_path;
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

void PathImplSkia::text(Utf8View string, Font const& font)
{
    SkTextUtils::GetPath(string.as_string().characters_without_null_termination(), string.as_string().length(), SkTextEncoding::kUTF8, last_point().x(), last_point().y(), as<ScaledFont>(font).skia_font(1), m_path.ptr());
}
NonnullOwnPtr<PathImpl> PathImplSkia::place_text_along(Utf8View text, Font const& font) const
{
    auto sk_font = as<ScaledFont>(font).skia_font(1);
    size_t const text_length = text.length();
    SkScalar x = 0;
    SkScalar y = 0;
    SkTextBlobBuilder builder;
    SkTextBlobBuilder::RunBuffer runBuffer = builder.allocRun(sk_font, text_length, x, y, nullptr);
    sk_font.textToGlyphs(text.as_string().characters_without_null_termination(), text.as_string().length(), SkTextEncoding::kUTF8, runBuffer.glyphs, text_length);
    SkPathMeasure pathMeasure(*m_path, false);
    SkScalar accumulated_distance = 0;
    auto output_path = PathImplSkia::create();
    for (size_t i = 0; i < text_length; ++i) {
        SkGlyphID glyph = runBuffer.glyphs[i];
        SkPath glyphPath;
        sk_font.getPath(glyph, &glyphPath);

        SkScalar advance;
        sk_font.getWidths(&glyph, 1, &advance);

        SkPoint position;
        SkVector tangent;
        if (!pathMeasure.getPosTan(accumulated_distance, &position, &tangent))
            continue;

        SkMatrix matrix;
        matrix.setTranslate(position.x(), position.y());
        matrix.preRotate(SkRadiansToDegrees(std::atan2(tangent.y(), tangent.x())));

        glyphPath.transform(matrix);
        output_path->sk_path().addPath(glyphPath);

        accumulated_distance += advance;
    }

    return output_path;
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
