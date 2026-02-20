/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/PainterSkia.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/OffscreenCanvasRenderingContext2DPrototype.h>
#include <LibWeb/HTML/AbstractCanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(OffscreenCanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<OffscreenCanvasRenderingContext2D>> OffscreenCanvasRenderingContext2D::create(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, JS::Value options)
{
    auto context_attributes = TRY(CanvasRenderingContext2DSettings::from_js_value(realm.vm(), options));
    return realm.create<OffscreenCanvasRenderingContext2D>(realm, offscreen_canvas, context_attributes);
}

OffscreenCanvasRenderingContext2D::OffscreenCanvasRenderingContext2D(JS::Realm& realm, OffscreenCanvas& offscreen_canvas, CanvasRenderingContext2DSettings context_attributes)
    : Bindings::PlatformObject(realm)
    , AbstractCanvasRenderingContext2D(static_cast<Bindings::PlatformObject&>(*this), offscreen_canvas, context_attributes)
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
    visitor.visit(m_element);
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

void OffscreenCanvasRenderingContext2D::set_filter(String)
{
    // Fixme: seems to need access to the dom
}

void OffscreenCanvasRenderingContext2D::did_draw(Gfx::FloatRect const&)
{
    // Do nothing the offscreen canvas doesnt need to be redrawn
}

void OffscreenCanvasRenderingContext2D::allocate_painting_surface_if_needed()
{
    if (m_surface || m_size.is_empty())
        return;

    auto color_type = m_context_attributes.alpha ? Gfx::BitmapFormat::BGRA8888 : Gfx::BitmapFormat::BGRx8888;
    // FIXME: throw some kind of error rather than crash if bitmap creation fails
    auto err_or_bitmap = Gfx::Bitmap::create(color_type, Gfx::AlphaType::Premultiplied, m_size);
    auto bitmap = MUST(err_or_bitmap);
    m_bitmap = Gfx::ShareableBitmap(bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap);
    m_surface = Gfx::PaintingSurface::wrap_bitmap(*m_bitmap.bitmap());

    // https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
    // Thus, the bitmap of such a context starts off as opaque black instead of transparent black;
    // AD-HOC: Skia provides us with a full transparent surface by default; only clear the surface if alpha is disabled.
    if (!m_context_attributes.alpha) {
        auto* painter = this->painter();
        painter->clear_rect(m_surface->rect().to_type<float>(), clear_color());
    }
}

[[nodiscard]] Gfx::Painter* OffscreenCanvasRenderingContext2D::painter()
{
    allocate_painting_surface_if_needed();
    auto surface = m_surface;
    if (!m_painter && surface) {
        m_painter = make<Gfx::PainterSkia>(*m_surface);
    }
    return m_painter.ptr();
}

}
