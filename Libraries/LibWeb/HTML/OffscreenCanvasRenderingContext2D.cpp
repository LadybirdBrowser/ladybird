/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/Rect.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OffscreenCanvasRenderingContext2DPrototype.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> OffscreenCanvasRenderingContext2D::create(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, JS::Value options)
{
    auto context_attributes = TRY(CanvasRenderingContext2DSettings::from_js_value(realm.vm(), options));
    return realm.create<OffscreenCanvasRenderingContext2D>(realm, offscreen_canvas, context_attributes);
}

OffscreenCanvasRenderingContext2D::OffscreenCanvasRenderingContext2D(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, CanvasRenderingContext2DSettings context_attributes)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_canvas(offscreen_canvas)
    , m_size(offscreen_canvas.bitmap_size_for_canvas())
    , m_context_attributes(context_attributes)
{
}

OffscreenCanvasRenderingContext2D::~OffscreenCanvasRenderingContext2D() = default;

void OffscreenCanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::OffscreenCanvasRenderingContext2DPrototype>(realm, "OffscreenCanvasRenderingContext2D"_string));
}

void OffscreenCanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas);
}

void OffscreenCanvasRenderingContext2D::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
}

GC::Ref<OffscreenCanvas> OffscreenCanvasRenderingContext2D::canvas()
{
    return m_canvas;
}

OffscreenCanvas& OffscreenCanvasRenderingContext2D::canvas_element()
{
    return *m_canvas;
}

OffscreenCanvas const& OffscreenCanvasRenderingContext2D::canvas_element() const
{

    return *m_canvas;
}

void OffscreenCanvasRenderingContext2D::fill_rect(float, float, float, float)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::fill_rect()");
}

void OffscreenCanvasRenderingContext2D::clear_rect(float, float, float, float)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clear_rect()");
}

void OffscreenCanvasRenderingContext2D::stroke_rect(float, float, float, float)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::stroke_rect()");
}

WebIDL::ExceptionOr<void> OffscreenCanvasRenderingContext2D::draw_image_internal(CanvasImageSource const&, float, float, float, float, float, float, float, float)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::draw_image_internal()");
    return {};
}

void OffscreenCanvasRenderingContext2D::begin_path()
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::begin_path()");
}

void OffscreenCanvasRenderingContext2D::stroke()
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::stroke()");
}

void OffscreenCanvasRenderingContext2D::stroke(Path2D const&)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::stroke(Path2D)");
}

void OffscreenCanvasRenderingContext2D::fill_text(Utf16String const&, float, float, Optional<double>)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::fill_text()");
}

void OffscreenCanvasRenderingContext2D::stroke_text(Utf16String const&, float, float, Optional<double>)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::stroke_text()");
}

void OffscreenCanvasRenderingContext2D::fill(StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::fill(StringView)");
}

void OffscreenCanvasRenderingContext2D::fill(Path2D&, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::fill(Path2D&, StringView)");
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(int, int, Optional<ImageDataSettings> const&) const
{
    return WebIDL::NotSupportedError::create(realm(), "(STUBBED) OffscreenCanvasRenderingContext2D::create_image_data(int, int)"_utf16);
}

WebIDL::ExceptionOr<GC::Ref<ImageData>> OffscreenCanvasRenderingContext2D::create_image_data(ImageData const&) const
{
    return WebIDL::NotSupportedError::create(realm(), "(STUBBED) OffscreenCanvasRenderingContext2D::create_image_data(ImageData&)"_utf16);
}

WebIDL::ExceptionOr<GC::Ptr<ImageData>> OffscreenCanvasRenderingContext2D::get_image_data(int, int, int, int, Optional<ImageDataSettings> const&) const
{
    return WebIDL::NotSupportedError::create(realm(), "(STUBBED) OffscreenCanvasRenderingContext2D::get_image_data()"_utf16);
}

void OffscreenCanvasRenderingContext2D::put_image_data(ImageData&, float, float)

{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::put_image_data()");
}

void OffscreenCanvasRenderingContext2D::reset_to_default_state()
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::reset_to_default_state()");
}

GC::Ref<TextMetrics> OffscreenCanvasRenderingContext2D::measure_text(Utf16String const&)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::measure_text()");

    auto metrics = TextMetrics::create(realm());
    return metrics;
}

void OffscreenCanvasRenderingContext2D::clip(StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(StringView)");
}

void OffscreenCanvasRenderingContext2D::clip(Path2D&, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(Path2D&, StringView)");
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(double, double, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::is_point_in_path(double, double, StringView)");
    return false;
}

bool OffscreenCanvasRenderingContext2D::is_point_in_path(Path2D const&, double, double, StringView)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::clip(Path2D const&, double, double, StringView)");
    return false;
}

bool OffscreenCanvasRenderingContext2D::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality OffscreenCanvasRenderingContext2D::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void OffscreenCanvasRenderingContext2D::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

String OffscreenCanvasRenderingContext2D::filter() const
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::filter()");
    return String::from_utf8_without_validation("none"sv.bytes());
}

void OffscreenCanvasRenderingContext2D::set_filter(String)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::set_filter()");
}

float OffscreenCanvasRenderingContext2D::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
void OffscreenCanvasRenderingContext2D::set_shadow_offset_x(float offset_x)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_x) || isnan(offset_x))
        return;

    drawing_state().shadow_offset_x = offset_x;
}

float OffscreenCanvasRenderingContext2D::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
void OffscreenCanvasRenderingContext2D::set_shadow_offset_y(float offset_y)
{
    // On setting, the attribute being set must be set to the new value, except if the value is infinite or NaN,
    // in which case the new value must be ignored.
    if (isinf(offset_y) || isnan(offset_y))
        return;

    drawing_state().shadow_offset_y = offset_y;
}

float OffscreenCanvasRenderingContext2D::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
void OffscreenCanvasRenderingContext2D::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

String OffscreenCanvasRenderingContext2D::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

void OffscreenCanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto style_value = parse_css_value(CSS::Parser::ParsingParams(), color, CSS::PropertyID::Color);
    if (style_value && style_value->has_color()) {
        auto parsedValue = style_value->to_color({}).value_or(Color::Black);

        // 4. Set this's shadow color to parsedValue.
        drawing_state().shadow_color = parsedValue;
    } else {
        // 3. If parsedValue is failure, then return.
        return;
    }
}

float OffscreenCanvasRenderingContext2D::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void OffscreenCanvasRenderingContext2D::set_global_alpha(float alpha)
{
    // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
    if (!isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return;
    }
    // 2. Otherwise, set this's global alpha to the given value.
    drawing_state().global_alpha = alpha;
}

String OffscreenCanvasRenderingContext2D::global_composite_operation() const
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::global_composite_operation()");
    return String::from_utf8_without_validation(""sv.bytes());
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void OffscreenCanvasRenderingContext2D::set_global_composite_operation(String)
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::set_global_composite_operation()");
}

[[nodiscard]] Gfx::Painter* OffscreenCanvasRenderingContext2D::painter()
{
    dbgln("(STUBBED) OffscreenCanvasRenderingContext2D::painter()");
    return nullptr;
}

}
