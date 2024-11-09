/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLOutputElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/HTML/HTMLOutputElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLOutputElement);

HTMLOutputElement::HTMLOutputElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOutputElement::~HTMLOutputElement() = default;

void HTMLOutputElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOutputElement);
}

void HTMLOutputElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_html_for);
}

void HTMLOutputElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value)
{
    if (name == HTML::AttributeNames::for_) {
        if (m_html_for)
            m_html_for->associated_attribute_changed(value.value_or(String {}));
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-output-htmlfor
JS::NonnullGCPtr<DOM::DOMTokenList> HTMLOutputElement::html_for()
{
    // The htmlFor IDL attribute must reflect the for content attribute.
    if (!m_html_for)
        m_html_for = DOM::DOMTokenList::create(*this, HTML::AttributeNames::for_);
    return *m_html_for;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-output-defaultvalue
String HTMLOutputElement::default_value() const
{
    // 1. If this element's default value override is non-null, then return it.
    if (m_default_value_override.has_value())
        return *m_default_value_override;

    // 2. Return this element's descendant text content.
    return descendant_text_content();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-output-defaultvalue
void HTMLOutputElement::set_default_value(String const& default_value)
{
    // 1. If this's default value override is null, then string replace all with the given value within this and return.
    if (!m_default_value_override.has_value()) {
        string_replace_all(default_value);
        return;
    }

    // 2. Set this's default value override to the given value.
    m_default_value_override = default_value;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-output-value
String HTMLOutputElement::value() const
{
    // The value getter steps are to return this's descendant text content.
    return descendant_text_content();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-output-value
void HTMLOutputElement::set_value(String const& value)
{
    // 1. Set this's default value override to its default value.
    m_default_value_override = default_value();

    // 2. String replace all with the given value within this.
    string_replace_all(value);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-output-element:concept-form-reset-control
void HTMLOutputElement::reset_algorithm()
{
    // 1. String replace all with this element's default value within this element.
    string_replace_all(default_value());

    // 2. Set this element's default value override to null.
    m_default_value_override = {};
}

// https://w3c.github.io/webdriver/#dfn-clear-algorithm
void HTMLOutputElement::clear_algorithm()
{
    // The clear algorithm for output elements is set the element's value mode flag to default
    m_default_value_override = default_value();

    // and then to set the element's textContent IDL attribute to an empty string (thus clearing the element's child nodes).
    string_replace_all({});
}

}
