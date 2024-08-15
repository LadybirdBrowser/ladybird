/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBodyElementPrototype.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLBodyElement);

HTMLBodyElement::HTMLBodyElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLBodyElement::~HTMLBodyElement() = default;

void HTMLBodyElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_background_style_value)
        m_background_style_value->visit_edges(visitor);
}

void HTMLBodyElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLBodyElement);
}

void HTMLBodyElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name.equals_ignoring_ascii_case("bgcolor"sv)) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                style.set_property(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name.equals_ignoring_ascii_case("text"sv)) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value-2
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                style.set_property(CSS::PropertyID::Color, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name.equals_ignoring_ascii_case("background"sv)) {
            VERIFY(m_background_style_value);
            style.set_property(CSS::PropertyID::BackgroundImage, *m_background_style_value);
        }
    });
}

void HTMLBodyElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, old_value, value);
    if (name.equals_ignoring_ascii_case("link"sv)) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value-3
        auto color = parse_legacy_color_value(value.value_or(String {}));
        if (color.has_value())
            document().set_normal_link_color(color.value());
    } else if (name.equals_ignoring_ascii_case("alink"sv)) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value-5
        auto color = parse_legacy_color_value(value.value_or(String {}));
        if (color.has_value())
            document().set_active_link_color(color.value());
    } else if (name.equals_ignoring_ascii_case("vlink"sv)) {
        // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value-4
        auto color = parse_legacy_color_value(value.value_or(String {}));
        if (color.has_value())
            document().set_visited_link_color(color.value());
    } else if (name.equals_ignoring_ascii_case("background"sv)) {
        m_background_style_value = CSS::ImageStyleValue::create(document().parse_url(value.value_or(String {})));
        m_background_style_value->on_animate = [this] {
            if (paintable()) {
                paintable()->set_needs_display();
            }
        };
    }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                     \
    if (name == HTML::AttributeNames::attribute_name) {             \
        element_event_handler_attribute_changed(event_name, value); \
    }
    ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
}

JS::GCPtr<DOM::EventTarget> HTMLBodyElement::global_event_handlers_to_event_target(FlyString const& event_name)
{
    // NOTE: This is a little weird, but IIUC document.body.onload actually refers to window.onload
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    if (DOM::is_window_reflecting_body_element_event_handler(event_name))
        return document().window();

    return *this;
}

JS::GCPtr<DOM::EventTarget> HTMLBodyElement::window_event_handlers_to_event_target()
{
    // All WindowEventHandlers on HTMLFrameSetElement (e.g. document.body.onrejectionhandled) are mapped to window.on{event}.
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    return document().window();
}

}
