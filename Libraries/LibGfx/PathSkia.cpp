/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/Span.h>
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
#include <core/SkPathBuilder.h>
#include <core/SkPathMeasure.h>
#include <core/SkTextBlob.h>
#include <pathops/SkPathOps.h>
#include <utils/SkParsePath.h>
#include <utils/SkTextUtils.h>

namespace Gfx {

static FloatPoint to_gfx_point(SkPoint const& point)
{
    return { point.x(), point.y() };
}

NonnullOwnPtr<Gfx::PathImplSkia> PathImplSkia::create()
{
    return adopt_own(*new PathImplSkia);
}

PathImplSkia::PathImplSkia()
    : m_path_builder(adopt_own(*new SkPathBuilder))
{
}

PathImplSkia::PathImplSkia(PathImplSkia const& other)
    : m_last_move_to(other.m_last_move_to)
    , m_has_current_point(other.m_has_current_point)
    , m_path_builder(adopt_own(*new SkPathBuilder(*other.m_path_builder)))
{
}

PathImplSkia::~PathImplSkia() = default;

SkPath const& PathImplSkia::sk_path() const
{
    if (!m_cached_path)
        m_cached_path = adopt_own(*new SkPath(m_path_builder->snapshot()));
    return *m_cached_path;
}

SkPathBuilder& PathImplSkia::sk_path_builder()
{
    m_cached_path = nullptr;
    return *m_path_builder;
}

void PathImplSkia::update_state_from_builder()
{
    update_state_from_path(sk_path());
}

void PathImplSkia::set_path(SkPath const& path)
{
    sk_path_builder() = SkPathBuilder(path);
    update_state_from_path(path);
}

void PathImplSkia::update_state_from_path(SkPath const& path)
{
    m_has_current_point = false;
    m_last_move_to = {};

    auto const points = path.points();
    size_t point_index = 0;
    for (auto verb : path.verbs()) {
        switch (verb) {
        case SkPathVerb::kMove:
            m_has_current_point = true;
            m_last_move_to = to_gfx_point(points[point_index++]);
            break;
        case SkPathVerb::kLine:
            m_has_current_point = true;
            point_index += 1;
            break;
        case SkPathVerb::kQuad:
        case SkPathVerb::kConic:
            m_has_current_point = true;
            point_index += 2;
            break;
        case SkPathVerb::kCubic:
            m_has_current_point = true;
            point_index += 3;
            break;
        case SkPathVerb::kClose:
            m_has_current_point = true;
            break;
        }
    }
}

void PathImplSkia::clear()
{
    sk_path_builder().reset();
    m_last_move_to = {};
    m_has_current_point = false;
}

void PathImplSkia::move_to(Gfx::FloatPoint const& point)
{
    m_last_move_to = point;
    m_has_current_point = true;
    sk_path_builder().moveTo(point.x(), point.y());
}

void PathImplSkia::line_to(Gfx::FloatPoint const& point)
{
    if (!m_has_current_point) {
        move_to(point);
        return;
    }
    sk_path_builder().lineTo(point.x(), point.y());
}

void PathImplSkia::close()
{
    if (!m_has_current_point)
        return;
    sk_path_builder().close();
    sk_path_builder().moveTo(m_last_move_to.x(), m_last_move_to.y());
}

void PathImplSkia::elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep)
{
    if (!m_has_current_point) {
        move_to(point);
        return;
    }

    SkPoint skPoint = SkPoint::Make(point.x(), point.y());
    SkPoint skRadii = SkPoint::Make(radii.width(), radii.height());
    SkScalar skXRotation = SkFloatToScalar(sk_float_radians_to_degrees(x_axis_rotation));
    SkPathBuilder::ArcSize skLargeArc = large_arc ? SkPathBuilder::kLarge_ArcSize : SkPathBuilder::kSmall_ArcSize;
    SkPathDirection skSweep = sweep ? SkPathDirection::kCW : SkPathDirection::kCCW;
    sk_path_builder().arcTo(skRadii, skXRotation, skLargeArc, skSweep, skPoint);
}

void PathImplSkia::arc_to(FloatPoint point, float radius, bool large_arc, bool sweep)
{
    if (!m_has_current_point) {
        move_to(point);
        return;
    }

    SkPoint skPoint = SkPoint::Make(point.x(), point.y());
    SkPoint skRadii = SkPoint::Make(radius, radius);
    SkPathBuilder::ArcSize skLargeArc = large_arc ? SkPathBuilder::kLarge_ArcSize : SkPathBuilder::kSmall_ArcSize;
    SkPathDirection skSweep = sweep ? SkPathDirection::kCW : SkPathDirection::kCCW;
    sk_path_builder().arcTo(skRadii, 0, skLargeArc, skSweep, skPoint);
}

void PathImplSkia::quadratic_bezier_curve_to(FloatPoint through, FloatPoint point)
{
    if (!m_has_current_point)
        move_to(through);
    sk_path_builder().quadTo(through.x(), through.y(), point.x(), point.y());
}

void PathImplSkia::cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2)
{
    if (!m_has_current_point)
        move_to(c1);
    sk_path_builder().cubicTo(c1.x(), c1.y(), c2.x(), c2.y(), p2.x(), p2.y());
}

void PathImplSkia::text(Utf8View const& string, Font const& font)
{
    SkPath text_path;
    SkTextUtils::GetPath(string.as_string().characters_without_null_termination(), string.as_string().length(), SkTextEncoding::kUTF8, last_point().x(), last_point().y(), font.skia_font(1), &text_path);
    set_path(text_path);
}

void PathImplSkia::text(Utf16View const& string, Font const& font)
{
    if (string.has_ascii_storage()) {
        text(Utf8View { string.bytes() }, font);
        return;
    }

    SkPath text_path;
    SkTextUtils::GetPath(string.utf16_span().data(), string.length_in_code_units() * sizeof(char16_t), SkTextEncoding::kUTF16, last_point().x(), last_point().y(), font.skia_font(1), &text_path);
    set_path(text_path);
}

void PathImplSkia::glyph_run(GlyphRun const& glyph_run)
{
    auto sk_font = glyph_run.font().skia_font(1);
    auto& path_builder = sk_path_builder();
    path_builder.setFillType(SkPathFillType::kWinding);
    auto font_ascent = glyph_run.font().pixel_metrics().ascent;
    for (auto const& glyph : glyph_run.glyphs()) {
        auto glyph_path = sk_font.getPath(static_cast<SkGlyphID>(glyph.glyph_id));
        if (!glyph_path.has_value())
            continue;
        path_builder.addPath(*glyph_path, glyph.position.x(), glyph.position.y() + font_ascent);
    }
    update_state_from_path(sk_path());
}

void PathImplSkia::offset(Gfx::FloatPoint const& offset)
{
    sk_path_builder().offset(offset.x(), offset.y());
    if (m_has_current_point)
        m_last_move_to.translate_by(offset);
}

template<typename TextToGlyphs>
static NonnullOwnPtr<PathImpl> place_text_along_impl(SkPath const& path, Font const& font, size_t length_in_code_points, float offset, TextToGlyphs&& text_to_glyphs)
{
    auto sk_font = font.skia_font(1);
    SkScalar x = 0;
    SkScalar y = 0;

    SkTextBlobBuilder builder;
    auto const& run_buffer = builder.allocRun(sk_font, static_cast<int>(length_in_code_points), x, y, nullptr);
    text_to_glyphs(sk_font, run_buffer);

    SkPathMeasure path_measure(path, false);
    SkScalar accumulated_distance = offset;

    auto output_path = PathImplSkia::create();
    SkScalar path_length = path_measure.getLength();

    for (size_t i = 0; i < length_in_code_points; ++i) {
        SkGlyphID glyph = run_buffer.glyphs[i];
        auto glyph_path = sk_font.getPath(glyph);

        SkScalar advance = 0;
        sk_font.getWidths({ &glyph, 1 }, { &advance, 1 });

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

        if (glyph_path.has_value())
            output_path->sk_path_builder().addPath(*glyph_path, matrix);

        accumulated_distance += advance;
    }
    output_path->update_state_from_builder();

    return output_path;
}

NonnullOwnPtr<PathImpl> PathImplSkia::place_text_along(Utf8View const& text, Font const& font, float offset) const
{
    auto length_in_code_points = text.length();

    return place_text_along_impl(sk_path(), font, length_in_code_points, offset, [&](auto const& sk_font, auto const& run_buffer) {
        sk_font.textToGlyphs(text.as_string().characters_without_null_termination(), text.as_string().length(), SkTextEncoding::kUTF8, { run_buffer.glyphs, length_in_code_points });
    });
}

NonnullOwnPtr<PathImpl> PathImplSkia::place_text_along(Utf16View const& text, Font const& font, float offset) const
{
    if (text.has_ascii_storage())
        return place_text_along(Utf8View { text.bytes() }, font, offset);

    auto length_in_code_points = text.length_in_code_points();

    return place_text_along_impl(sk_path(), font, length_in_code_points, offset, [&](auto const& sk_font, auto const& run_buffer) {
        sk_font.textToGlyphs(text.utf16_span().data(), text.length_in_code_units() * sizeof(char16_t), SkTextEncoding::kUTF16, { run_buffer.glyphs, length_in_code_points });
    });
}

void PathImplSkia::append_path(Gfx::Path const& other)
{
    auto const& other_impl = static_cast<PathImplSkia const&>(other.impl());
    sk_path_builder().addPath(other_impl.sk_path());
    if (other_impl.m_has_current_point) {
        m_has_current_point = true;
        m_last_move_to = other_impl.m_last_move_to;
    }
}

void PathImplSkia::intersect(Gfx::Path const& other)
{
    auto result = Op(sk_path(), static_cast<PathImplSkia const&>(other.impl()).sk_path(), SkPathOp::kIntersect_SkPathOp);
    if (result.has_value())
        set_path(*result);
}

Vector<u8> PathImplSkia::serialize_to_bytes() const
{
    auto const& path = sk_path();
    auto path_data_size = path.writeToMemory(nullptr);
    Vector<u8> path_data;
    path_data.resize(path_data_size);
    path.writeToMemory(path_data.data());
    return path_data;
}

void PathImplSkia::deserialize_from_bytes(ReadonlyBytes bytes)
{
    set_path(SkPath::ReadFromMemory(bytes.data(), bytes.size()).value_or(SkPath {}));
}

bool PathImplSkia::is_empty() const
{
    return !m_has_current_point;
}

Gfx::FloatPoint PathImplSkia::last_point() const
{
    auto last = m_path_builder->getLastPt();
    if (!last.has_value())
        return {};
    return { last->fX, last->fY };
}

Gfx::FloatRect PathImplSkia::bounding_box() const
{
    auto bounds = m_path_builder->computeBounds();
    return { bounds.fLeft, bounds.fTop, bounds.fRight - bounds.fLeft, bounds.fBottom - bounds.fTop };
}

float PathImplSkia::length() const
{
    SkPathMeasure path_measure(sk_path(), false);
    return path_measure.getLength();
}

bool PathImplSkia::contains(FloatPoint point, Gfx::WindingRule winding_rule) const
{
    SkPath temp_path = sk_path();
    temp_path.setFillType(to_skia_path_fill_type(winding_rule));
    return temp_path.contains(point.x(), point.y());
}

void PathImplSkia::set_fill_type(Gfx::WindingRule winding_rule)
{
    sk_path_builder().setFillType(to_skia_path_fill_type(winding_rule));
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
    new_path->sk_path_builder().transform(matrix);
    if (new_path->m_has_current_point)
        new_path->m_last_move_to = transform.map(new_path->m_last_move_to);
    return new_path;
}

String PathImplSkia::to_svg_string() const
{
    auto svg_string = SkParsePath::ToSVGString(sk_path());
    return MUST(String::from_utf8(StringView { svg_string.c_str(), svg_string.size() }));
}

}
