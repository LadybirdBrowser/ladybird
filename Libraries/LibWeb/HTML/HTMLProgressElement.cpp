/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLProgressElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLProgressElement);

HTMLProgressElement::HTMLProgressElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLProgressElement::~HTMLProgressElement() = default;

void HTMLProgressElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLProgressElement);
}

void HTMLProgressElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_progress_value_element);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-progress-value
double HTMLProgressElement::value() const
{
    if (auto value_string = get_attribute(HTML::AttributeNames::value); value_string.has_value()) {
        if (auto value = parse_floating_point_number(*value_string); value.has_value())
            return clamp(*value, 0, max());
    }
    return 0;
}

WebIDL::ExceptionOr<void> HTMLProgressElement::set_value(double value)
{
    if (value < 0)
        value = 0;

    TRY(set_attribute(HTML::AttributeNames::value, String::number(value)));
    update_progress_value_element();
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-progress-max
WebIDL::Double HTMLProgressElement::max() const
{
    if (auto max_string = get_attribute(HTML::AttributeNames::max); max_string.has_value()) {
        if (auto max = parse_floating_point_number(*max_string); max.has_value())
            if (*max > 0)
                return *max;
    }
    return 1;
}

WebIDL::ExceptionOr<void> HTMLProgressElement::set_max(double value)
{
    if (value <= 0)
        return {};

    TRY(set_attribute(HTML::AttributeNames::max, String::number(value)));
    update_progress_value_element();
    return {};
}

double HTMLProgressElement::position() const
{
    if (!is_determinate())
        return -1;

    return value() / max();
}

void HTMLProgressElement::inserted()
{
    Base::inserted();
    create_shadow_tree_if_needed();
}

void HTMLProgressElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    Base::removed_from(old_parent, old_root);
    set_shadow_root(nullptr);
}

void HTMLProgressElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

void HTMLProgressElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    auto progress_bar_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    progress_bar_element->set_use_pseudo_element(CSS::PseudoElement::Track);
    MUST(shadow_root->append_child(*progress_bar_element));

    m_progress_value_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_progress_value_element->set_use_pseudo_element(CSS::PseudoElement::Fill);
    MUST(progress_bar_element->append_child(*m_progress_value_element));
    update_progress_value_element();
}

void HTMLProgressElement::update_progress_value_element()
{
    if (m_progress_value_element)
        MUST(m_progress_value_element->style_for_bindings()->set_property(CSS::PropertyID::Width, MUST(String::formatted("{}%", position() * 100))));
}

}
