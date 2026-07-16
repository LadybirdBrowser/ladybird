/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CanvasRenderingContext2D.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<CanvasRenderingContext2D>> CanvasRenderingContext2D::create(JS::Realm& realm, HTMLCanvasElement& element, JS::Value options)
{
    auto context_attributes = TRY(Bindings::convert_to_idl_value_for_canvas_rendering_context2d_settings(realm.vm(), options));
    return realm.create<CanvasRenderingContext2D>(realm, element, context_attributes);
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element, Bindings::CanvasRenderingContext2DSettings context_attributes)
    : Canvas2DContextBase(realm, element.bitmap_size_for_canvas(), move(context_attributes))
    , m_element(element)
{
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::CanvasRenderingContext2DPrototype>(realm, "CanvasRenderingContext2D"_utf16_fly_string));
}

void CanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

GC::Ref<HTMLCanvasElement> CanvasRenderingContext2D::canvas_for_binding() const
{
    return *m_element;
}

void CanvasRenderingContext2D::did_draw_hook()
{
    m_element->set_canvas_content_dirty();
    m_element->set_needs_repaint(InvalidateDisplayList::No);
}

Page* CanvasRenderingContext2D::page_for_compositor()
{
    return &m_element->document().page();
}

void CanvasRenderingContext2D::backing_storage_created_hook()
{
    m_element->set_needs_repaint();
}

DOM::EventTarget& CanvasRenderingContext2D::context_event_target()
{
    return *m_element;
}

Gfx::Color CanvasRenderingContext2D::resolve_drop_shadow_color(CSS::DropShadowFilterStyleValue const& drop_shadow) const
{
    DOM::AbstractElement abstract_element { *m_element };
    m_element->document().update_style_if_needed_for_element(abstract_element);

    Gfx::Color color = Gfx::Color::Black;
    if (drop_shadow.color() && m_element->computed_values()) {
        auto color_context = CSS::ColorResolutionContext::for_element(abstract_element);
        color = drop_shadow.color()->to_color(color_context).value_or(Gfx::Color::Black);
    }
    return color;
}

}
