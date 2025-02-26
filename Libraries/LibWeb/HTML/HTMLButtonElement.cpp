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

    // The attribute's missing value default and invalid value default are both the Auto state.
    // https://html.spec.whatwg.org/multipage/form-elements.html#attr-button-type-auto-state
    return HTMLButtonElement::TypeAttributeState::Auto;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-button-type
String HTMLButtonElement::type_for_bindings() const
{
    // The type getter steps are:
    // 1. If this is a submit button, then return "submit".
    if (is_submit_button())
        return "submit"_string;

    // 2. Let state be this's type attribute.
    auto state = type_state();

    // 3. Assert: state is not in the Submit Button state.
    VERIFY(state != TypeAttributeState::Submit);

    // 4. If state is in the Auto state, then return "button".
    if (state == TypeAttributeState::Auto)
        return "button"_string;

    // 5. Return the keyword value corresponding to state.
    switch (state) {
#define __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(keyword, state) \
    case TypeAttributeState::state:                            \
        return #keyword##_string;
        ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-button-type
WebIDL::ExceptionOr<void> HTMLButtonElement::set_type_for_bindings(String const& type)
{
    // The type setter steps are to set the type content attribute to the given value.
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
    // A button element is said to be a submit button if any of the following are true:
    switch (type_state()) {
        // - the type attribute is in the Auto state and both the command and commandfor content attributes are not present; or
    case TypeAttributeState::Auto:
        return !has_attribute(AttributeNames::command) && !has_attribute(AttributeNames::commandfor);
        // - the type attribute is in the Submit Button state.
    case TypeAttributeState::Submit:
        return true;
    default:
        return false;
    }
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

    // 3. If element has a form owner:
    if (form() != nullptr) {
        // 1. If element is a submit button, then submit element's form owner from element with userInvolvement set to event's user navigation involvement, and return.
        if (is_submit_button()) {
            form()->submit_form(*this, { .user_involvement = user_navigation_involvement(event) }).release_value_but_fixme_should_propagate_errors();
            return;
        }
        // 2. If element's type attribute is in the Reset Button state, then reset element's form owner, and return.
        if (type_state() == TypeAttributeState::Reset) {
            form()->reset_form();
            return;
        }
        // 3. If element's type attribute is in the Auto state, then return.
        if (type_state() == TypeAttributeState::Auto)
            return;
    }

    // FIXME: 4. Let target be the result of running element's get the commandfor associated element.
    // FIXME: 5. If target is not null:
    //           ...

    // 6. Otherwise, run the popover target attribute activation behavior given element and event's target.
    if (event.target() && event.target()->is_dom_node())
        PopoverInvokerElement::popover_target_activation_behaviour(*this, as<DOM::Node>(*event.target()));
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
bool HTMLButtonElement::will_validate()
{
    // The willValidate attribute's getter must return true, if this element is a candidate for constraint validation
    return is_candidate_for_constraint_validation();
}

bool HTMLButtonElement::is_focusable() const
{
    return enabled();
}

}
