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
#include <LibGfx/TextLayout.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

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

void Path::clear()
{
    FFI::path_geometry_clear(m_geometry);
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

void Path::elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep)
{
    FFI::path_geometry_elliptical_arc_to(m_geometry, point.x(), point.y(), radii.width(), radii.height(), x_axis_rotation, large_arc, sweep);
}

void Path::arc_to(FloatPoint point, float radius, bool large_arc, bool sweep)
{
    FFI::path_geometry_arc_to(m_geometry, point.x(), point.y(), radius, large_arc, sweep);
}

void Path::quadratic_bezier_curve_to(FloatPoint through, FloatPoint point)
{
    FFI::path_geometry_quadratic_bezier_curve_to(m_geometry, through.x(), through.y(), point.x(), point.y());
}

void Path::cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2)
{
    FFI::path_geometry_cubic_bezier_curve_to(m_geometry, c1.x(), c1.y(), c2.x(), c2.y(), p2.x(), p2.y());
}

void Path::append_path(Path const& other)
{
    FFI::path_geometry_append_path(m_geometry, other.m_geometry);
}

static void append_to_byte_vector(void* context, u8 const* bytes, size_t count)
{
    static_cast<Vector<u8>*>(context)->append(bytes, count);
}

Vector<u8> Path::serialize_to_bytes() const
{
    Vector<u8> bytes;
    serialize_into(append_to_byte_vector, &bytes);
    return bytes;
}

void Path::serialize_into(void (*append)(void*, u8 const*, size_t), void* context) const
{
    FFI::path_geometry_encode(m_geometry, append, context);
}

bool Path::is_empty() const
{
    return FFI::path_geometry_is_empty(m_geometry);
}

FloatPoint Path::last_point() const
{
    float x_y[2];
    FFI::path_geometry_last_point(m_geometry, x_y);
    return { x_y[0], x_y[1] };
}

FloatRect Path::bounding_box() const
{
    float x_y_width_height[4];
    FFI::path_geometry_bounding_box(m_geometry, x_y_width_height);
    return { x_y_width_height[0], x_y_width_height[1], x_y_width_height[2], x_y_width_height[3] };
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

Path Path::stroke_to_fill(StrokeStyle const& style) const
{
    return Path { FFI::path_geometry_stroke_to_fill(m_geometry, style.thickness, to_underlying(style.cap_style), to_underlying(style.join_style), style.miter_limit, style.dash_array.data(), style.dash_array.size(), style.dash_offset) };
}

void Path::set_fill_type(WindingRule winding_rule)
{
    FFI::path_geometry_set_fill_rule(m_geometry, to_underlying(winding_rule));
}

static Array<float, 6> affine_values(AffineTransform const& transform)
{
    return { transform.a(), transform.b(), transform.c(), transform.d(), transform.e(), transform.f() };
}

Path Path::copy_transformed(AffineTransform const& transform) const
{
    auto values = affine_values(transform);
    return Path { FFI::path_geometry_copy_transformed(m_geometry, values.data()) };
}

void Path::transform(AffineTransform const& transform)
{
    auto values = affine_values(transform);
    FFI::path_geometry_transform(m_geometry, values.data());
}

String Path::to_svg_string() const
{
    String string;
    FFI::path_geometry_svg_string(
        m_geometry, [](void* context, u8 const* bytes, size_t count) {
            *static_cast<String*>(context) = String::from_utf8_without_validation({ bytes, count });
        },
        &string);
    return string;
}

PathData Path::data() const
{
    FFI::PathGeometryData data;
    FFI::path_geometry_data(m_geometry, &data);
    return {
        .verbs = { reinterpret_cast<PathVerb const*>(data.verbs), data.verb_count },
        .points = { reinterpret_cast<FloatPoint const*>(data.points), data.point_count },
        .conic_weights = { data.conic_weights, data.conic_weight_count },
        .winding_rule = static_cast<WindingRule>(data.fill_rule),
    };
}

bool Path::operator==(Path const& other) const
{
    return FFI::path_geometry_equals(m_geometry, other.m_geometry);
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

    // Appends the outline of the glyph to the path, mapped by the affine transform with the values a, b, c, d, e and f.
    void append_outline(u32 glyph_id, FFI::PathGeometry* path, Array<float, 6> const& transform) const
    {
        auto* outline = FFI::path_geometry_create();
        ScopeGuard destroy_outline = [&] { FFI::path_geometry_destroy(outline); };
        draw_outline(glyph_id, outline);
        FFI::path_geometry_append_transformed_contours(path, outline, transform.data());
    }

private:
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

static Font const& font_of(FFI::FfiGlyphRun const& glyph_run)
{
    return *static_cast<Font const*>(glyph_run.font);
}

static ReadonlySpan<DrawGlyph> glyphs_of(FFI::FfiGlyphRun const& glyph_run)
{
    return { reinterpret_cast<DrawGlyph const*>(glyph_run.glyphs), glyph_run.glyph_count };
}

void Path::glyph_run(FFI::FfiGlyphRun const& glyph_run)
{
    auto const& font = font_of(glyph_run);
    if (font.is_invisible())
        return;
    FFI::path_geometry_set_fill_rule(m_geometry, to_underlying(WindingRule::Nonzero));
    auto font_ascent = font.pixel_metrics().ascent;
    GlyphOutliner outliner { font };
    for (auto const& glyph : glyphs_of(glyph_run))
        outliner.append_outline(glyph.glyph_id, m_geometry, { 1, 0, 0, 1, glyph.position.x(), glyph.position.y() + font_ascent });
    FFI::path_geometry_update_state_from_contours(m_geometry);
}

// Lays each glyph along the first subpath that has length, with its origin at the distance of its x position plus the
// offset and its baseline along the direction of the path there, until a glyph's middle would go past the end.
Path Path::place_glyph_runs_along(ReadonlySpan<FFI::FfiGlyphRun> glyph_runs, float offset) const
{
    Path output;
    auto* measure = FFI::path_geometry_measure_first_contour(m_geometry);
    if (!measure)
        return output;
    ScopeGuard destroy_measure = [&] { FFI::contour_measure_destroy(measure); };
    auto path_length = FFI::contour_measure_length(measure);

    auto append_glyph_outlines = [&] {
        for (auto const& glyph_run : glyph_runs) {
            auto const& font = font_of(glyph_run);
            if (font.is_invisible())
                continue;
            GlyphOutliner outliner { font };
            for (auto const& glyph : glyphs_of(glyph_run)) {
                auto glyph_distance = offset + glyph.position.x();
                if (glyph_distance + (glyph.glyph_width / 2.0f) > path_length)
                    return;

                float point_and_tangent[4];
                FFI::contour_measure_position_and_tangent(measure, glyph_distance, point_and_tangent);
                auto [x, y, tangent_x, tangent_y] = point_and_tangent;
                outliner.append_outline(glyph.glyph_id, output.m_geometry, { tangent_x, tangent_y, -tangent_y, tangent_x, x, y });
            }
        }
    };
    append_glyph_outlines();
    FFI::path_geometry_update_state_from_contours(output.m_geometry);
    return output;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::Path const& path)
{
    return encoder.encode(path.serialize_to_bytes());
}

template<>
ErrorOr<Gfx::Path> decode(Decoder& decoder)
{
    auto path_data = TRY(decoder.decode<Vector<u8>>());
    return Gfx::Path::from_serialized_bytes(path_data);
}

}

// The functions through which Rust code outside LibGfx builds and queries paths. See Rust/src/path.rs.

extern "C" {
void ladybird_gfx_path_destroy(void*);
bool ladybird_gfx_path_equals(void const*, void const*);
void ladybird_gfx_path_append_svg_string(void const*, void (*)(void*, u8 const*, size_t), void*);
void ladybird_gfx_path_bounding_box(void const*, float*);
void ladybird_gfx_path_serialize(void const*, void (*)(void*, u8 const*, size_t), void*);
float ladybird_gfx_path_length(void const*);
bool ladybird_gfx_path_point_at_length(void const*, float, float*);
void* ladybird_gfx_path_create_from_ops(u8 const* kinds, float const* values, size_t count);
void ladybird_gfx_path_append_ops(void*, u8 const* kinds, float const* values, size_t count);
void ladybird_gfx_path_append_path(void*, void const* other);
bool ladybird_gfx_path_is_empty(void const*);
void ladybird_gfx_path_last_point(void const*, float* out_x_y);
void* ladybird_gfx_path_clone(void const*);
void* ladybird_gfx_path_create_from_serialized_bytes(u8 const* bytes, size_t count);
void* ladybird_gfx_path_copy_transformed(void const*, float const* affine_values);
bool ladybird_gfx_path_contains(void const*, float, float, i32);
void* ladybird_gfx_path_stroke_to_fill(void const*, float thickness, i32 cap_style, i32 join_style, float miter_limit, float const* dash_array, size_t dash_count, float dash_offset);
void ladybird_gfx_path_set_fill_type(void*, i32);
void* ladybird_gfx_path_create_from_glyph_runs(Gfx::FFI::FfiGlyphRun const* runs, size_t run_count);
void* ladybird_gfx_path_place_glyph_runs_along(void const* path, Gfx::FFI::FfiGlyphRun const* runs, size_t run_count, float offset);
}

extern "C" void ladybird_gfx_path_destroy(void* path)
{
    delete static_cast<Gfx::Path*>(path);
}

extern "C" void ladybird_gfx_path_append_svg_string(void const* path, void (*append)(void*, u8 const*, size_t), void* context)
{
    auto string = static_cast<Gfx::Path const*>(path)->to_svg_string();
    append(context, string.bytes().data(), string.bytes().size());
}

extern "C" void ladybird_gfx_path_set_fill_type(void* path, i32 winding_rule)
{
    static_cast<Gfx::Path*>(path)->set_fill_type(static_cast<Gfx::WindingRule>(winding_rule));
}

extern "C" void ladybird_gfx_path_bounding_box(void const* path, float* out_x_y_width_height)
{
    auto rect = static_cast<Gfx::Path const*>(path)->bounding_box();
    out_x_y_width_height[0] = rect.x();
    out_x_y_width_height[1] = rect.y();
    out_x_y_width_height[2] = rect.width();
    out_x_y_width_height[3] = rect.height();
}

extern "C" void ladybird_gfx_path_serialize(void const* path, void (*append)(void*, u8 const*, size_t), void* context)
{
    static_cast<Gfx::Path const*>(path)->serialize_into(append, context);
}

extern "C" float ladybird_gfx_path_length(void const* path)
{
    return static_cast<Gfx::Path const*>(path)->length();
}

extern "C" bool ladybird_gfx_path_point_at_length(void const* path, float distance, float* out)
{
    auto point = static_cast<Gfx::Path const*>(path)->point_at_length(distance);
    if (!point.has_value())
        return false;
    out[0] = point->point.x();
    out[1] = point->point.y();
    out[2] = point->tangent.x();
    out[3] = point->tangent.y();
    return true;
}

enum class PathBuilderOpKind : u8 {
    MoveTo,
    LineTo,
    QuadraticBezierCurveTo,
    CubicBezierCurveTo,
    EllipticalArcTo,
    ArcTo,
    Close,
};

extern "C" void* ladybird_gfx_path_create_from_ops(u8 const* kinds, float const* values, size_t count)
{
    auto* path = new Gfx::Path;
    ladybird_gfx_path_append_ops(path, kinds, values, count);
    return path;
}

extern "C" void ladybird_gfx_path_append_ops(void* raw_path, u8 const* kinds, float const* values, size_t count)
{
    auto* path = static_cast<Gfx::Path*>(raw_path);
    for (size_t i = 0; i < count; ++i) {
        auto const* v = values + i * 7;
        switch (static_cast<PathBuilderOpKind>(kinds[i])) {
        case PathBuilderOpKind::MoveTo:
            path->move_to({ v[0], v[1] });
            break;
        case PathBuilderOpKind::LineTo:
            path->line_to({ v[0], v[1] });
            break;
        case PathBuilderOpKind::QuadraticBezierCurveTo:
            path->quadratic_bezier_curve_to({ v[0], v[1] }, { v[2], v[3] });
            break;
        case PathBuilderOpKind::CubicBezierCurveTo:
            path->cubic_bezier_curve_to({ v[0], v[1] }, { v[2], v[3] }, { v[4], v[5] });
            break;
        case PathBuilderOpKind::EllipticalArcTo:
            path->elliptical_arc_to({ v[0], v[1] }, { v[2], v[3] }, v[4], v[5] != 0, v[6] != 0);
            break;
        case PathBuilderOpKind::ArcTo:
            path->arc_to({ v[0], v[1] }, v[2], v[3] != 0, v[4] != 0);
            break;
        case PathBuilderOpKind::Close:
            path->close();
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

extern "C" void ladybird_gfx_path_append_path(void* path, void const* other)
{
    static_cast<Gfx::Path*>(path)->append_path(*static_cast<Gfx::Path const*>(other));
}

extern "C" bool ladybird_gfx_path_is_empty(void const* path)
{
    return static_cast<Gfx::Path const*>(path)->is_empty();
}

extern "C" void ladybird_gfx_path_last_point(void const* path, float* out_x_y)
{
    auto point = static_cast<Gfx::Path const*>(path)->last_point();
    out_x_y[0] = point.x();
    out_x_y[1] = point.y();
}

extern "C" void* ladybird_gfx_path_clone(void const* path)
{
    return new Gfx::Path(static_cast<Gfx::Path const*>(path)->clone());
}

extern "C" void* ladybird_gfx_path_create_from_glyph_runs(Gfx::FFI::FfiGlyphRun const* runs, size_t run_count)
{
    auto* path = new Gfx::Path;
    for (auto const& glyph_run : ReadonlySpan<Gfx::FFI::FfiGlyphRun> { runs, run_count })
        path->glyph_run(glyph_run);
    return path;
}

extern "C" void* ladybird_gfx_path_place_glyph_runs_along(void const* path, Gfx::FFI::FfiGlyphRun const* runs, size_t run_count, float offset)
{
    return new Gfx::Path(static_cast<Gfx::Path const*>(path)->place_glyph_runs_along({ runs, run_count }, offset));
}

extern "C" void* ladybird_gfx_path_create_from_serialized_bytes(u8 const* bytes, size_t count)
{
    return new Gfx::Path(Gfx::Path::from_serialized_bytes({ bytes, count }));
}

extern "C" void* ladybird_gfx_path_copy_transformed(void const* path, float const* affine_values)
{
    Gfx::AffineTransform transform { affine_values[0], affine_values[1], affine_values[2], affine_values[3], affine_values[4], affine_values[5] };
    return new Gfx::Path(static_cast<Gfx::Path const*>(path)->copy_transformed(transform));
}

extern "C" bool ladybird_gfx_path_contains(void const* path, float x, float y, i32 winding_rule)
{
    return static_cast<Gfx::Path const*>(path)->contains({ x, y }, static_cast<Gfx::WindingRule>(winding_rule));
}

extern "C" void* ladybird_gfx_path_stroke_to_fill(void const* path, float thickness, i32 cap_style, i32 join_style, float miter_limit, float const* dash_array, size_t dash_count, float dash_offset)
{
    Gfx::Path::StrokeStyle style {
        .thickness = thickness,
        .cap_style = static_cast<Gfx::Path::CapStyle>(cap_style),
        .join_style = static_cast<Gfx::Path::JoinStyle>(join_style),
        .miter_limit = miter_limit,
        .dash_array = { dash_array, dash_count },
        .dash_offset = dash_offset,
    };
    return new Gfx::Path(static_cast<Gfx::Path const*>(path)->stroke_to_fill(style));
}

extern "C" bool ladybird_gfx_path_equals(void const* a, void const* b)
{
    return *static_cast<Gfx::Path const*>(a) == *static_cast<Gfx::Path const*>(b);
}
