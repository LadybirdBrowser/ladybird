/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLBodyElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLBodyElement);

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

bool HTMLBodyElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::bgcolor,
        HTML::AttributeNames::text,
        HTML::AttributeNames::background);
}

void HTMLBodyElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    for_each_attribute([&](auto& name, auto& value) {
        if (name.equals_ignoring_ascii_case("bgcolor"sv)) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundColor, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name.equals_ignoring_ascii_case("text"sv)) {
            // https://html.spec.whatwg.org/multipage/rendering.html#the-page:rules-for-parsing-a-legacy-colour-value-2
            auto color = parse_legacy_color_value(value);
            if (color.has_value())
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Color, CSS::CSSColorValue::create_from_color(color.value()));
        } else if (name.equals_ignoring_ascii_case("background"sv)) {
            VERIFY(m_background_style_value);
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BackgroundImage, *m_background_style_value);
        }
    });

    auto get_margin_value = [&](auto const& first_body_attr_name, auto const& second_body_attr_name, auto const& container_frame_attr_name) -> Optional<String> {
        if (auto value = get_attribute(first_body_attr_name); value.has_value())
            return value.value();
        if (auto value = get_attribute(second_body_attr_name); value.has_value())
            return value.value();
        auto navigable = document().navigable();
        if (!navigable)
            return {};
        auto container = navigable->container();
        if (!container)
            return {};
        if (auto value = container->get_attribute(container_frame_attr_name); value.has_value())
            return value;
        return {};
    };
    auto margin_top_value = get_margin_value(HTML::AttributeNames::marginheight, HTML::AttributeNames::topmargin, HTML::AttributeNames::marginheight);
    auto margin_bottom_value = get_margin_value(HTML::AttributeNames::marginheight, HTML::AttributeNames::bottommargin, HTML::AttributeNames::marginheight);
    auto margin_left_value = get_margin_value(HTML::AttributeNames::marginwidth, HTML::AttributeNames::leftmargin, HTML::AttributeNames::marginwidth);
    auto margin_right_value = get_margin_value(HTML::AttributeNames::marginwidth, HTML::AttributeNames::rightmargin, HTML::AttributeNames::marginwidth);

    auto apply_margin_value = [&](CSS::PropertyID property_id, Optional<String> const& value) {
        if (!value.has_value())
            return;
        if (auto parsed_value = parse_non_negative_integer(value.value()); parsed_value.has_value())
            cascaded_properties->set_property_from_presentational_hint(property_id, CSS::LengthStyleValue::create(CSS::Length::make_px(*parsed_value)));
    };

    apply_margin_value(CSS::PropertyID::MarginTop, margin_top_value);
    apply_margin_value(CSS::PropertyID::MarginBottom, margin_bottom_value);
    apply_margin_value(CSS::PropertyID::MarginLeft, margin_left_value);
    apply_margin_value(CSS::PropertyID::MarginRight, margin_right_value);
}

void HTMLBodyElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

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
        // https://html.spec.whatwg.org/multipage/rendering.html#the-page:attr-background
        if (auto maybe_background_url = document().encoding_parse_url(value.value_or(String {})); maybe_background_url.has_value()) {
            m_background_style_value = CSS::ImageStyleValue::create(maybe_background_url.value());
            m_background_style_value->on_animate = [this] {
                if (paintable())
                    paintable()->set_needs_display();
            };
        }
    }

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                     \
    if (name == HTML::AttributeNames::attribute_name) {             \
        element_event_handler_attribute_changed(event_name, value); \
    }
    ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE
}

GC::Ptr<DOM::EventTarget> HTMLBodyElement::global_event_handlers_to_event_target(FlyString const& event_name)
{
    // NOTE: This is a little weird, but IIUC document.body.onload actually refers to window.onload
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    if (DOM::is_window_reflecting_body_element_event_handler(event_name))
        return document().window();

    return *this;
}

GC::Ptr<DOM::EventTarget> HTMLBodyElement::window_event_handlers_to_event_target()
{
    // All WindowEventHandlers on HTMLFrameSetElement (e.g. document.body.onrejectionhandled) are mapped to window.on{event}.
    // NOTE: document.body can return either a HTMLBodyElement or HTMLFrameSetElement, so both these elements must support this mapping.
    return document().window();
}

}
