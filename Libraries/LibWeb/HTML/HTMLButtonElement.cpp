/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLButtonElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLButtonElement);

HTMLButtonElement::HTMLButtonElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLButtonElement::~HTMLButtonElement() = default;

void HTMLButtonElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLButtonElement);
}

HTMLButtonElement::TypeAttributeState HTMLButtonElement::type_state() const
{
    auto value = get_attribute_value(HTML::AttributeNames::type);

#define __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(keyword, state) \
    if (value.equals_ignoring_ascii_case(#keyword##sv))        \
        return HTMLButtonElement::TypeAttributeState::state;
    ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE

    // The missing value default and invalid value default are the Submit Button state.
    return HTMLButtonElement::TypeAttributeState::Submit;
}

WebIDL::ExceptionOr<void> HTMLButtonElement::set_type(String const& type)
{
    return set_attribute(HTML::AttributeNames::type, type);
}

void HTMLButtonElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    PopoverInvokerElement::associated_attribute_changed(name, value, namespace_);
}

void HTMLButtonElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    PopoverInvokerElement::visit_edges(visitor);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLButtonElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

// https://html.spec.whatwg.org/multipage/forms.html#concept-submit-button
// https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:concept-submit-button
bool HTMLButtonElement::is_submit_button() const
{
    // If the type attribute is in the Submit Button state, the element is specifically a submit button.
    return type_state() == TypeAttributeState::Submit;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:concept-fe-value
String HTMLButtonElement::value() const
{
    return attribute(AttributeNames::value).value_or(String {});
}

bool HTMLButtonElement::has_activation_behavior() const
{
    return true;
}

void HTMLButtonElement::activation_behavior(DOM::Event const& event)
{
    // https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:activation-behaviour
    // 1. If element is disabled, then return.
    if (!enabled())
        return;

    // 2. If element's node document is not fully active, then return.
    if (!this->document().is_fully_active())
        return;

    // 3. If element has a form owner then switch on element's type attribute's state, then:
    if (form() != nullptr) {
        switch (type_state()) {
        case TypeAttributeState::Submit:
            // Submit Button
            // Submit element's form owner from element with userInvolvement set to event's user navigation involvement.
            form()->submit_form(*this, { .user_involvement = user_navigation_involvement(event) }).release_value_but_fixme_should_propagate_errors();
            break;
        case TypeAttributeState::Reset:
            // Reset Button
            // Reset element's form owner.
            form()->reset_form();
            break;
        case TypeAttributeState::Button:
            // Button
            // Do nothing.
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    // 4. Run the popover target attribute activation behavior given element and event's target.
    if (event.target() && event.target()->is_dom_node())
        PopoverInvokerElement::popover_target_activation_behaviour(*this, as<DOM::Node>(*event.target()));
}

bool HTMLButtonElement::is_focusable() const
{
    return enabled();
}

}
