/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <AK/TypeCasts.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Path.h>
#include <LibGfx/RustFFI.h>

#include <harfbuzz/hb.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/Font/TypefaceCoreText.h>
#endif

static_assert(sizeof(Gfx::PathVertex) == sizeof(Gfx::FFI::PathVertex));
static_assert(offsetof(Gfx::PathVertex, point) == offsetof(Gfx::FFI::PathVertex, point));
static_assert(offsetof(Gfx::PathVertex, distance) == offsetof(Gfx::FFI::PathVertex, distance));
static_assert(offsetof(Gfx::PathVertex, direction) == offsetof(Gfx::FFI::PathVertex, direction));
static_assert(sizeof(Gfx::FloatPoint) == sizeof(Gfx::FFI::FloatPoint));
static_assert(to_underlying(Gfx::WindingRule::Nonzero) == 0 && to_underlying(Gfx::WindingRule::EvenOdd) == 1);
static_assert(to_underlying(Gfx::PathVerb::Move) == to_underlying(Gfx::FFI::PathVerb::Move));
static_assert(to_underlying(Gfx::PathVerb::Line) == to_underlying(Gfx::FFI::PathVerb::Line));
static_assert(to_underlying(Gfx::PathVerb::Quad) == to_underlying(Gfx::FFI::PathVerb::Quad));
static_assert(to_underlying(Gfx::PathVerb::Conic) == to_underlying(Gfx::FFI::PathVerb::Conic));
static_assert(to_underlying(Gfx::PathVerb::Cubic) == to_underlying(Gfx::FFI::PathVerb::Cubic));
static_assert(to_underlying(Gfx::PathVerb::Close) == to_underlying(Gfx::FFI::PathVerb::Close));

namespace Gfx {

Path::Path()
    : m_geometry(FFI::path_geometry_create())
{
}

Path::Path(FFI::PathGeometry* geometry)
    : m_geometry(geometry)
{
}

Path::~Path()
{
    FFI::path_geometry_destroy(m_geometry);
}

Path::Path(Path const& other)
    : m_geometry(FFI::path_geometry_clone(other.m_geometry))
{
}

Path& Path::operator=(Path const& other)
{
    if (this != &other) {
        FFI::path_geometry_destroy(m_geometry);
        m_geometry = FFI::path_geometry_clone(other.m_geometry);
    }
    return *this;
}

Path::Path(Path&& other)
    : m_geometry(exchange(other.m_geometry, nullptr))
{
}

Path& Path::operator=(Path&& other)
{
    if (this != &other)
        swap(m_geometry, other.m_geometry);
    return *this;
}

Path Path::from_serialized_bytes(ReadonlyBytes bytes)
{
    return Path { FFI::path_geometry_decode(bytes.data(), bytes.size()) };
}

void Path::move_to(FloatPoint const& point)
{
    FFI::path_geometry_move_to(m_geometry, point.x(), point.y());
}

void Path::line_to(FloatPoint const& point)
{
    FFI::path_geometry_line_to(m_geometry, point.x(), point.y());
}

void Path::close()
{
    FFI::path_geometry_close(m_geometry);
}

void Path::arc_to(FloatPoint point, float radius, bool large_arc, bool sweep)
{
    FFI::path_geometry_arc_to(m_geometry, point.x(), point.y(), radius, large_arc, sweep);
}

void Path::quadratic_bezier_curve_to(FloatPoint through, FloatPoint point)
{
    FFI::path_geometry_quadratic_bezier_curve_to(m_geometry, through.x(), through.y(), point.x(), point.y());
}

Vector<u8> Path::serialize_to_bytes() const
{
    Vector<u8> bytes;
    FFI::path_geometry_encode(
        m_geometry, [](void* context, u8 const* bytes, size_t count) {
            static_cast<Vector<u8>*>(context)->append(bytes, count);
        },
        &bytes);
    return bytes;
}

float Path::length() const
{
    return FFI::path_geometry_length(m_geometry);
}

Optional<PointAlongPath> Path::point_at_length(float distance) const
{
    float point_and_tangent[4];
    if (!FFI::path_geometry_point_at_length(m_geometry, distance, point_and_tangent))
        return {};
    return PointAlongPath { { point_and_tangent[0], point_and_tangent[1] }, { point_and_tangent[2], point_and_tangent[3] } };
}

Vector<PathVertex> Path::vertices() const
{
    Vector<PathVertex> vertices;
    FFI::path_geometry_vertices(
        m_geometry, [](void* context, FFI::PathVertex const* ffi_vertices, size_t count) {
            static_cast<Vector<PathVertex>*>(context)->append(reinterpret_cast<PathVertex const*>(ffi_vertices), count);
        },
        &vertices);
    return vertices;
}

bool Path::contains(FloatPoint point, WindingRule winding_rule) const
{
    return FFI::path_geometry_contains(m_geometry, point.x(), point.y(), to_underlying(winding_rule));
}

PathData PathData::from_ffi(FFI::PathGeometryData const& data)
{
    return {
        .verbs = { reinterpret_cast<PathVerb const*>(data.verbs), data.verb_count },
        .points = { reinterpret_cast<FloatPoint const*>(data.points), data.point_count },
        .conic_weights = { data.conic_weights, data.conic_weight_count },
        .winding_rule = static_cast<WindingRule>(data.fill_rule),
    };
}

PathData Path::data() const
{
    FFI::PathGeometryData data;
    FFI::path_geometry_data(m_geometry, &data);
    return PathData::from_ffi(data);
}

namespace {

// Maps the coordinates of a glyph outline from font units, with y up, to pixels, with y down. A contour starts only at
// its first segment that goes somewhere, and segments that stay at one point are left out, so contours of a single
// point, which fonts use as anchors, add nothing to the path.
struct GlyphOutline {
    FFI::PathGeometry* outline;
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
            FFI::path_geometry_contour_move_to(outline, x(current_x), y(current_y));
        }
        current_x = to_x;
        current_y = to_y;
    }

    void close_contour()
    {
        if (!has_contour)
            return;
        FFI::path_geometry_contour_close(outline);
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
                FFI::path_geometry_contour_line_to(outline.outline, outline.x(to_x), outline.y(to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float control_x, float control_y, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                if (outline.is_at(control_x, control_y) && outline.is_at(to_x, to_y))
                    return;
                outline.go_to(to_x, to_y);
                FFI::path_geometry_contour_quad_to(outline.outline, outline.x(control_x), outline.y(control_y), outline.x(to_x), outline.y(to_y));
            },
            nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(
            funcs, [](hb_draw_funcs_t*, void* draw_data, hb_draw_state_t*, float control1_x, float control1_y, float control2_x, float control2_y, float to_x, float to_y, void*) {
                auto& outline = *static_cast<GlyphOutline*>(draw_data);
                if (outline.is_at(control1_x, control1_y) && outline.is_at(control2_x, control2_y) && outline.is_at(to_x, to_y))
                    return;
                outline.go_to(to_x, to_y);
                FFI::path_geometry_contour_cubic_to(outline.outline, outline.x(control1_x), outline.y(control1_y), outline.x(control2_x), outline.y(control2_y), outline.x(to_x), outline.y(to_y));
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

#ifdef AK_OS_MACOS
void append_core_text_glyph_outline(FFI::PathGeometry* outline, CTFontRef font, u32 glyph_id)
{
    auto path = CTFontCreatePathForGlyph(font, static_cast<CGGlyph>(glyph_id), nullptr);
    if (!path)
        return;
    ScopeGuard release_path = [&] { CGPathRelease(path); };

    // CoreText gives points in pixels with y up.
    CGPathApply(path, outline, [](void* info, CGPathElement const* element) {
        auto* outline = static_cast<FFI::PathGeometry*>(info);
        auto x = [&](size_t index) { return static_cast<float>(element->points[index].x); };
        auto y = [&](size_t index) { return 0.0f - static_cast<float>(element->points[index].y); };
        switch (element->type) {
        case kCGPathElementMoveToPoint:
            FFI::path_geometry_contour_move_to(outline, x(0), y(0));
            break;
        case kCGPathElementAddLineToPoint:
            FFI::path_geometry_contour_line_to(outline, x(0), y(0));
            break;
        case kCGPathElementAddQuadCurveToPoint:
            FFI::path_geometry_contour_quad_to(outline, x(0), y(0), x(1), y(1));
            break;
        case kCGPathElementAddCurveToPoint:
            FFI::path_geometry_contour_cubic_to(outline, x(0), y(0), x(1), y(1), x(2), y(2));
            break;
        case kCGPathElementCloseSubpath:
            FFI::path_geometry_contour_close(outline);
            break;
        }
    });
}
#endif

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

    void draw_outline(u32 glyph_id, FFI::PathGeometry* outline) const
    {
#ifdef AK_OS_MACOS
        if (m_core_text_font) {
            append_core_text_glyph_outline(outline, m_core_text_font, glyph_id);
            return;
        }
#endif

        auto* hb_font = m_font.harfbuzz_font();
        int x_scale = 0;
        int y_scale = 0;
        hb_font_get_scale(hb_font, &x_scale, &y_scale);
        if (x_scale <= 0 || y_scale <= 0)
            return;
        GlyphOutline glyph_outline {
            .outline = outline,
            .units_to_pixels_x = m_font.pixel_size() / static_cast<float>(x_scale),
            .units_to_pixels_y = m_font.pixel_size() / static_cast<float>(y_scale),
        };
        hb_font_draw_glyph(hb_font, glyph_id, glyph_outline_draw_funcs(), &glyph_outline);
        glyph_outline.close_contour();
    }

    Font const& m_font;
#ifdef AK_OS_MACOS
    // The font at its size and variations, for outlines that only CoreText draws.
    CTFontRef m_core_text_font { nullptr };
#endif
};

}

}

// The function through which Rust code builds glyph outlines into paths. See Rust/src/path.rs.

extern "C" {
void ladybird_gfx_font_glyph_outlines(void const* font, Gfx::FFI::DrawGlyph const* glyphs, size_t glyph_count, void (*visit)(void*, size_t, Gfx::FFI::PathGeometryData const*), void* context);
}

// Calls the callback with the outline of each glyph, in pixels with its origin on the baseline.
extern "C" void ladybird_gfx_font_glyph_outlines(void const* font, Gfx::FFI::DrawGlyph const* glyphs, size_t glyph_count, void (*visit)(void*, size_t, Gfx::FFI::PathGeometryData const*), void* context)
{
    Gfx::GlyphOutliner outliner { *static_cast<Gfx::Font const*>(font) };
    auto* outline = Gfx::FFI::path_geometry_create();
    ScopeGuard destroy_outline = [&] { Gfx::FFI::path_geometry_destroy(outline); };
    for (size_t index = 0; index < glyph_count; ++index) {
        Gfx::FFI::path_geometry_clear(outline);
        outliner.draw_outline(glyphs[index].glyph_id, outline);
        Gfx::FFI::PathGeometryData data;
        Gfx::FFI::path_geometry_data(outline, &data);
        visit(context, index, &data);
    }
}
