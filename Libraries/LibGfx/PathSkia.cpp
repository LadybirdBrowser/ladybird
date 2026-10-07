/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AK_DONT_REPLACE_STD

#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <AK/Noncopyable.h>
#include <AK/ScopeGuard.h>
#include <AK/Span.h>
#include <AK/TypeCasts.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/PathSkia.h>
#include <LibGfx/Rect.h>
#include <LibGfx/TextLayout.h>
#include <core/SkPath.h>
#include <core/SkPathBuilder.h>
#include <core/SkPathMeasure.h>
#include <core/SkPathUtils.h>
#include <core/SkString.h>
#include <effects/SkDashPathEffect.h>
#include <utils/SkParsePath.h>

#include <harfbuzz/hb.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif

template<>
constexpr bool AllocatedWithSystemAllocator<SkPath> = true;

template<>
constexpr bool AllocatedWithSystemAllocator<SkPathBuilder> = true;

namespace Gfx {

static FloatPoint to_gfx_point(SkPoint const& point)
{
    return { point.x(), point.y() };
}

static SkPathFillType to_skia_path_fill_type(WindingRule winding_rule)
{
    switch (winding_rule) {
    case WindingRule::Nonzero:
        return SkPathFillType::kWinding;
    case WindingRule::EvenOdd:
        return SkPathFillType::kEvenOdd;
    }
    VERIFY_NOT_REACHED();
}

namespace {

// Maps the coordinates of a glyph outline from font units, with y up, to pixels, with y down. A contour starts only at
// its first segment that goes somewhere, and segments that stay at one point are left out, so contours of a single
// point, which fonts use as anchors, add nothing to the path.
struct GlyphOutline {
    SkPathBuilder& builder;
    float units_to_pixels_x;
    float units_to_pixels_y;
    bool has_contour { false };
    float current_x { 0 };
    float current_y { 0 };

    float x(float units) const { return units * units_to_pixels_x; }
    // Subtract from 0, so that a point on the baseline gets 0 and not -0.
    float y(float units) const { return 0.0f - units * units_to_pixels_y; }

    bool is_at(float to_x, float to_y) const { return current_x == to_x && current_y == to_y; }

    void go_to(float to_x, float to_y)
    {
        if (!has_contour) {
            has_contour = true;
            builder.moveTo(x(current_x), y(current_y));
        }
        current_x = to_x;
        current_y = to_y;
    }

    void close_contour()
    {
        if (!has_contour)
            return;
        builder.close();
        has_contour = false;
    }
};

// NOTE: HarfBuzz emits the implicit line that closes each contour as a regular line_to before close_path. The
//       quadratic callback must be set, otherwise HarfBuzz converts quadratic segments to cubics.
hb_draw_funcs_t* glyph_outline_draw_funcs()
{
    static hb_draw_funcs_t* draw_funcs = [] {
        auto* funcs = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                outline.close_contour();
                outline.current_x = to_x;
                outline.current_y = to_y;
            },
            nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                if (outline.is_at(to_x, to_y))
                    return;
                outline.go_to(to_x, to_y);
                outline.builder.lineTo(outline.x(to_x), outline.y(to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float control_x, float control_y, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                if (outline.is_at(control_x, control_y) && outline.is_at(to_x, to_y))
                    return;
                outline.go_to(to_x, to_y);
                outline.builder.quadTo(outline.x(control_x), outline.y(control_y), outline.x(to_x), outline.y(to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float control1_x, float control1_y, float control2_x, float control2_y, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                if (outline.is_at(control1_x, control1_y) && outline.is_at(control2_x, control2_y) && outline.is_at(to_x, to_y))
                    return;
                outline.go_to(to_x, to_y);
                outline.builder.cubicTo(outline.x(control1_x), outline.y(control1_y), outline.x(control2_x), outline.y(control2_y), outline.x(to_x), outline.y(to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_close_path_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, void*) {
                static_cast<GlyphOutline*>(draw_data)->close_contour();
            },
            nullptr, nullptr);
        hb_draw_funcs_make_immutable(funcs);
        return funcs;
    }();
    return draw_funcs;
}

}

#ifdef AK_OS_MACOS
static SkPath core_text_glyph_outline(CTFontRef font, u32 glyph_id)
{
    auto path = CTFontCreatePathForGlyph(font, static_cast<CGGlyph>(glyph_id), nullptr);
    if (!path)
        return {};
    ScopeGuard release_path = [&] { CGPathRelease(path); };

    // CoreText gives points in pixels with y up.
    SkPathBuilder builder;
    CGPathApply(path, &builder, [](void* info, CGPathElement const* element) {
        auto& builder = *static_cast<SkPathBuilder*>(info);
        auto point = [&](size_t index) {
            return SkPoint::Make(static_cast<float>(element->points[index].x), 0.0f - static_cast<float>(element->points[index].y));
        };
        switch (element->type) {
        case kCGPathElementMoveToPoint:
            builder.moveTo(point(0));
            break;
        case kCGPathElementAddLineToPoint:
            builder.lineTo(point(0));
            break;
        case kCGPathElementAddQuadCurveToPoint:
            builder.quadTo(point(0), point(1));
            break;
        case kCGPathElementAddCurveToPoint:
            builder.cubicTo(point(0), point(1), point(2));
            break;
        case kCGPathElementCloseSubpath:
            builder.close();
            break;
        }
    });
    return builder.detach();
}
#endif

namespace {

// Gives the outlines of the glyphs of a font in pixels, with their origin on the baseline.
class GlyphOutliner {
    AK_MAKE_NONCOPYABLE(GlyphOutliner);
    AK_MAKE_NONMOVABLE(GlyphOutliner);

public:
    explicit GlyphOutliner(Font const& font)
        : m_font(font)
    {
#ifdef AK_OS_MACOS
        if (auto const* core_text_typeface = as_if<TypefaceCoreText>(font.typeface()); core_text_typeface && core_text_typeface->has_outlines_that_only_core_text_draws())
            m_core_text_font = create_core_text_font_with_variations(core_text_typeface->core_text_font(), font.pixel_size(), font.variation_settings().to_sorted_list());
#endif
    }

    ~GlyphOutliner()
    {
#ifdef AK_OS_MACOS
        if (m_core_text_font)
            CFRelease(m_core_text_font);
#endif
    }

    SkPath outline(u32 glyph_id) const
    {
#ifdef AK_OS_MACOS
        if (m_core_text_font)
            return core_text_glyph_outline(m_core_text_font, glyph_id);
#endif

        auto* hb_font = m_font.harfbuzz_font();
        int x_scale = 0;
        int y_scale = 0;
        hb_font_get_scale(hb_font, &x_scale, &y_scale);
        if (x_scale <= 0 || y_scale <= 0)
            return {};
        SkPathBuilder builder;
        GlyphOutline outline {
            .builder = builder,
            .units_to_pixels_x = m_font.pixel_size() / static_cast<float>(x_scale),
            .units_to_pixels_y = m_font.pixel_size() / static_cast<float>(y_scale),
        };
        hb_font_draw_glyph(hb_font, glyph_id, glyph_outline_draw_funcs(), &outline);
        outline.close_contour();
        return builder.detach();
    }

private:
    Font const& m_font;
#ifdef AK_OS_MACOS
    // The font at its size and variations, for outlines that only CoreText draws.
    CTFontRef m_core_text_font { nullptr };
#endif
};

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

PathImplSkia::~PathImplSkia()
{
    delete m_cached_path.load(AK::memory_order_relaxed);
}

SkPath const& PathImplSkia::sk_path() const
{
    if (auto* cached_path = m_cached_path.load(AK::memory_order_acquire))
        return *cached_path;
    auto* path = new SkPath(m_path_builder->snapshot());
    SkPath* expected = nullptr;
    if (m_cached_path.compare_exchange_strong(expected, path, AK::memory_order_acq_rel))
        return *path;
    delete path;
    return *expected;
}

SkPathBuilder& PathImplSkia::sk_path_builder()
{
    delete m_cached_path.exchange(nullptr, AK::memory_order_relaxed);
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

void PathImplSkia::glyph_run(GlyphRun const& glyph_run)
{
    if (glyph_run.font().is_invisible())
        return;
    auto& path_builder = sk_path_builder();
    path_builder.setFillType(SkPathFillType::kWinding);
    auto font_ascent = glyph_run.font().pixel_metrics().ascent;
    GlyphOutliner outliner { glyph_run.font() };
    for (auto const& glyph : glyph_run.glyphs())
        path_builder.addPath(outliner.outline(glyph.glyph_id), glyph.position.x(), glyph.position.y() + font_ascent);
    update_state_from_path(sk_path());
}

// By default, Skia measures a curve by the lengths of the lines between points along it that are at most half a unit off
// the curve, which leaves the length of a circle with a radius of 25 units about 1 unit short. Measure curves sixteen
// times as precisely instead.
static constexpr SkScalar path_measure_resolution_scale = 16;

// The furthest that the lines between points along a curve stay off it when Skia measures the curve.
static constexpr float path_measure_tolerance = 0.5f / path_measure_resolution_scale;

// The most lines with which to follow a single curve, which bounds the number of vertices of a huge curve.
static constexpr u32 maximum_line_count_along_curve = 4096;

NonnullOwnPtr<PathImpl> PathImplSkia::place_glyph_runs_along(ReadonlySpan<NonnullRefPtr<GlyphRun>> glyph_runs, float offset) const
{
    SkPathMeasure path_measure(sk_path(), false, path_measure_resolution_scale);
    SkScalar path_length = path_measure.getLength();

    auto output_path = PathImplSkia::create();

    bool reached_end_of_path = false;
    for (auto const& glyph_run : glyph_runs) {
        if (glyph_run->font().is_invisible())
            continue;
        GlyphOutliner outliner { glyph_run->font() };
        for (auto const& glyph : glyph_run->glyphs()) {
            SkScalar glyph_distance = offset + glyph.position.x();

            SkPoint position;
            SkVector tangent;
            if (!path_measure.getPosTan(glyph_distance, &position, &tangent))
                continue;

            SkScalar midpoint_distance = glyph_distance + (glyph.glyph_width / 2.0f);
            if (midpoint_distance > path_length) {
                reached_end_of_path = true;
                break;
            }

            SkMatrix matrix;
            matrix.setTranslate(position.x(), position.y());
            matrix.preRotate(SkRadiansToDegrees(std::atan2(tangent.y(), tangent.x())));
            output_path->sk_path_builder().addPath(outliner.outline(glyph.glyph_id), matrix);
        }
        if (reached_end_of_path)
            break;
    }
    output_path->update_state_from_builder();

    return output_path;
}

static SkPathBuilder copy_without_last_verb(SkPathBuilder const& builder)
{
    auto const verbs = builder.verbs();
    auto const points = builder.points();
    auto const conic_weights = builder.conicWeights();
    SkPathBuilder copy(builder.fillType());
    size_t point_index = 0;
    size_t conic_weight_index = 0;
    for (size_t verb_index = 0; verb_index + 1 < verbs.size(); ++verb_index) {
        switch (verbs[verb_index]) {
        case SkPathVerb::kMove:
            copy.moveTo(points[point_index]);
            point_index += 1;
            break;
        case SkPathVerb::kLine:
            copy.lineTo(points[point_index]);
            point_index += 1;
            break;
        case SkPathVerb::kQuad:
            copy.quadTo(points[point_index], points[point_index + 1]);
            point_index += 2;
            break;
        case SkPathVerb::kConic:
            copy.conicTo(points[point_index], points[point_index + 1], conic_weights[conic_weight_index++]);
            point_index += 2;
            break;
        case SkPathVerb::kCubic:
            copy.cubicTo(points[point_index], points[point_index + 1], points[point_index + 2]);
            point_index += 3;
            break;
        case SkPathVerb::kClose:
            copy.close();
            break;
        }
    }
    return copy;
}

void PathImplSkia::append_path(Gfx::Path const& other)
{
    auto const& other_impl = static_cast<PathImplSkia const&>(other.impl());
    auto const other_path = other_impl.sk_path();
    auto& path_builder = sk_path_builder();
    // The other path starts with a move. If this path ends with a move, the result has two moves in sequence. That is
    // not a valid path, and the compositor gets an empty path. Remove the move first. A subpath with one point is not
    // filled or stroked.
    auto const verbs = path_builder.verbs();
    if (!other_path.isEmpty() && verbs.size() > 1 && verbs.back() == SkPathVerb::kMove)
        path_builder = copy_without_last_verb(path_builder);
    path_builder.addPath(other_path);
    if (other_impl.m_has_current_point) {
        m_has_current_point = true;
        m_last_move_to = other_impl.m_last_move_to;
    }
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
    SkPathMeasure path_measure(sk_path(), false, path_measure_resolution_scale);
    float length = 0;
    do {
        length += path_measure.getLength();
    } while (path_measure.nextContour());
    return length;
}

// The point at a distance along the path, clamped to its ends, and the unit tangent of the path there. The gaps between
// the subpaths do not add to the distance. A path without length is only its start point, where it has no tangent.
Optional<PointAlongPath> PathImplSkia::point_at_length(float distance) const
{
    SkPathMeasure path_measure(sk_path(), false, path_measure_resolution_scale);
    Optional<PointAlongPath> end_of_path;
    auto remaining_distance = max(distance, 0.f);
    do {
        auto subpath_length = path_measure.getLength();
        if (subpath_length <= 0)
            continue;
        SkPoint position;
        SkVector tangent;
        auto distance_in_subpath = min(remaining_distance, subpath_length);
        if (!path_measure.getPosTan(distance_in_subpath, &position, &tangent))
            continue;
        end_of_path = PointAlongPath { { position.x(), position.y() }, { tangent.x(), tangent.y() } };
        if (remaining_distance <= subpath_length)
            return end_of_path;
        remaining_distance -= subpath_length;
    } while (path_measure.nextContour());

    if (end_of_path.has_value())
        return end_of_path;
    if (sk_path().countPoints() == 0)
        return {};
    auto start = sk_path().getPoint(0);
    return PointAlongPath { { start.x(), start.y() }, { 0, 0 } };
}

// The number of lines with which to start following a curve, by Wang's formula: the square root of the largest second
// difference of its control points, scaled by its degree, over the tolerance. That keeps lines between points at equal
// parameter steps within the tolerance of the curve, and depends on how much the curve bends rather than on how long it
// is.
static u32 line_count_to_follow_curve(SkPathIter::Rec const& segment)
{
    auto const& points = segment.fPoints;
    auto second_difference = [&](size_t index) {
        return SkPoint::Length(points[index].x() - 2 * points[index + 1].x() + points[index + 2].x(),
            points[index].y() - 2 * points[index + 1].y() + points[index + 2].y());
    };

    float scaled_second_difference = 0;
    switch (segment.fVerb) {
    case SkPathVerb::kQuad:
        scaled_second_difference = second_difference(0) / 4;
        break;
    case SkPathVerb::kConic:
        // A conic with a weight of at most 1, like the arcs of SVG paths, bends no more than the quadratic with the same
        // control points.
        scaled_second_difference = max(1.f, segment.conicWeight()) * second_difference(0) / 4;
        break;
    case SkPathVerb::kCubic:
        scaled_second_difference = 3 * max(second_difference(0), second_difference(1)) / 4;
        break;
    default:
        return 1;
    }
    auto line_count = AK::ceil(AK::sqrt(static_cast<double>(scaled_second_difference) / path_measure_tolerance));
    return static_cast<u32>(clamp(line_count, 1., static_cast<double>(maximum_line_count_along_curve)));
}

struct CurveToFollow {
    SkPathMeasure& measure;
    float distance_to_curve;
    Vector<PathVertex>& vertices;
    u32 remaining_line_count;
};

// Appends the vertices between two points along a curve, splitting the line between them in half for as long as the
// curve passes further than the tolerance from it halfway, or until the curve has as many lines as it may have.
static void follow_curve_between(CurveToFollow& curve, float start_distance, FloatPoint start_point, float end_distance, FloatPoint end_point)
{
    if (curve.remaining_line_count == 0)
        return;
    auto middle_distance = (start_distance + end_distance) / 2;
    SkPoint position;
    SkVector tangent;
    if (!curve.measure.getPosTan(middle_distance, &position, &tangent))
        return;
    FloatPoint middle_point { position.x(), position.y() };
    if (middle_point.distance_from((start_point + end_point) / 2) <= path_measure_tolerance)
        return;

    --curve.remaining_line_count;
    follow_curve_between(curve, start_distance, start_point, middle_distance, middle_point);
    curve.vertices.append({ middle_point, curve.distance_to_curve + middle_distance, { tangent.x(), tangent.y() } });
    follow_curve_between(curve, middle_distance, middle_point, end_distance, end_point);
}

// The vertices of polylines through the ends of the segments of the path, with their distance along it. Along a curved
// segment, the polylines pass through points along the curve that keep them within path_measure_tolerance of it. The
// gaps between the subpaths do not add to the distance, so a subpath starts at the distance its predecessor ends at. A
// path without length is only its start point.
Vector<PathVertex> PathImplSkia::vertices() const
{
    auto to_point = [](SkPoint point) { return FloatPoint { point.x(), point.y() }; };

    // Each vertex has the unit tangent of the path there as its direction. Where the path changes direction at once,
    // the vertex appears twice: with the direction in which the path arrives, and with the one in which it leaves. A
    // subpath start that no segment with length leaves from has no direction.
    Vector<PathVertex> vertices;
    float distance = 0;
    auto iterator = sk_path().iter();
    while (auto segment = iterator.next()) {
        auto const& points = segment->fPoints;
        SkPathBuilder segment_builder;
        segment_builder.moveTo(points[0]);
        switch (segment->fVerb) {
        case SkPathVerb::kMove:
            vertices.append({ to_point(points[0]), distance, {} });
            continue;
        case SkPathVerb::kLine:
        case SkPathVerb::kClose:
            segment_builder.lineTo(points[1]);
            break;
        case SkPathVerb::kQuad:
            segment_builder.quadTo(points[1], points[2]);
            break;
        case SkPathVerb::kConic:
            segment_builder.conicTo(points[1], points[2], segment->conicWeight());
            break;
        case SkPathVerb::kCubic:
            segment_builder.cubicTo(points[1], points[2], points[3]);
            break;
        }

        // A segment without length has no direction, and leaves the path where it was.
        SkPathMeasure segment_measure(segment_builder.detach(), false, path_measure_resolution_scale);
        auto segment_length = segment_measure.getLength();
        if (segment_length <= 0)
            continue;
        auto direction_at = [&](float distance_in_segment) {
            SkVector tangent;
            if (!segment_measure.getPosTan(distance_in_segment, nullptr, &tangent))
                return FloatVector2 {};
            return FloatVector2 { tangent.x(), tangent.y() };
        };

        auto start_direction = direction_at(0);
        if (vertices.last().direction == FloatVector2 {})
            vertices.last().direction = start_direction;
        else if (vertices.last().direction != start_direction)
            vertices.append({ to_point(points[0]), distance, start_direction });

        auto line_count = line_count_to_follow_curve(*segment);
        CurveToFollow curve { segment_measure, distance, vertices, maximum_line_count_along_curve - line_count };
        auto previous_distance = 0.f;
        auto previous_point = to_point(points[0]);
        for (u32 line = 1; line <= line_count; ++line) {
            auto distance_in_segment = segment_length * static_cast<float>(line) / static_cast<float>(line_count);
            SkPoint position;
            SkVector tangent;
            if (line == line_count)
                position = points.back();
            else if (!segment_measure.getPosTan(distance_in_segment, &position, &tangent))
                continue;
            follow_curve_between(curve, previous_distance, previous_point, distance_in_segment, to_point(position));
            if (line < line_count)
                vertices.append({ to_point(position), distance + distance_in_segment, { tangent.x(), tangent.y() } });
            previous_distance = distance_in_segment;
            previous_point = to_point(position);
        }
        distance += segment_length;
        vertices.append({ to_point(points.back()), distance, direction_at(segment_length) });
    }

    // Skia's iterator leaves out a move that ends the path.
    if (vertices.is_empty() && sk_path().countPoints() > 0)
        vertices.append({ to_point(sk_path().getPoint(0)), 0, {} });
    return vertices;
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

NonnullOwnPtr<PathImplSkia> PathImplSkia::stroke_to_fill(Path::StrokeStyle const& style) const
{
    auto outline = PathImplSkia::create();
    // NB: Skia treats zero thickness as a hairline, which covers no area.
    if (!(style.thickness > 0))
        return outline;

    SkPaint paint;
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(style.thickness);
    paint.setStrokeCap(to_skia_cap(style.cap_style));
    paint.setStrokeJoin(to_skia_join(style.join_style));
    paint.setStrokeMiter(style.miter_limit);
    if (!style.dash_array.is_empty())
        paint.setPathEffect(SkDashPathEffect::Make({ style.dash_array.data(), style.dash_array.size() }, style.dash_offset));
    skpathutils::FillPathWithPaint(sk_path(), paint, &outline->sk_path_builder());
    outline->sk_path_builder().setFillType(SkPathFillType::kWinding);
    outline->update_state_from_builder();
    return outline;
}

Gfx::Path Path::stroke_to_fill(StrokeStyle const& style) const
{
    return Gfx::Path { static_cast<PathImplSkia const&>(impl()).stroke_to_fill(style) };
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
