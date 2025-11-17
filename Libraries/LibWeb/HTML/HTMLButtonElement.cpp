/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLButtonElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/CommandEvent.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLButtonElement);

HTMLButtonElement::HTMLButtonElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLButtonElement::~HTMLButtonElement() = default;

void HTMLButtonElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLButtonElement);
    Base::initialize(realm);
}

void HTMLButtonElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
    // If the computed value of 'display' is 'inline-grid', 'grid', 'inline-flex', 'flex', 'none', or 'contents', then behave as the computed value.
    auto display = style.display();
    if (display.is_flex_inside() || display.is_grid_inside() || display.is_none() || display.is_contents()) {
        // No-op
    } else if (display.is_inline_outside()) {
        // Otherwise, if the computed value of 'display' is a value such that the outer display type is 'inline', then behave as 'inline-block'.
        // AD-HOC: See https://github.com/whatwg/html/issues/11857
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));
    } else {
        // Otherwise, behave as 'flow-root'.
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::FlowRoot)));
    }
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
void HTMLButtonElement::set_type_for_bindings(String const& type)
{
    // The type setter steps are to set the type content attribute to the given value.
    set_attribute_value(HTML::AttributeNames::type, type);
}

void HTMLButtonElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const&, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    PopoverInvokerElement::associated_attribute_changed(name, value, namespace_);
}

void HTMLButtonElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    PopoverInvokerElement::visit_edges(visitor);
    visitor.visit(m_command_for_element);
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
Utf16String HTMLButtonElement::value() const
{
    // The element's value is the value of the element's value attribute, if there is one; otherwise the empty string.
    return Utf16String::from_utf8(attribute(AttributeNames::value).value_or(String {}));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:concept-fe-optional-value
Optional<String> HTMLButtonElement::optional_value() const
{
    // The element's optional value is the value of the element's value attribute, if there is one; otherwise null.
    return attribute(AttributeNames::value);
}

bool HTMLButtonElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:activation-behaviour
void HTMLButtonElement::activation_behavior(DOM::Event const& event)
{
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

    // 4. Let target be the result of running element's get the commandfor-associated element.
    //    AD-HOC: Target needs to be an HTML Element in the following steps.
    GC::Ptr<HTMLElement> target = as_if<HTMLElement>(m_command_for_element.ptr());
    if (!target) {
        auto target_id = attribute(AttributeNames::commandfor);
        if (target_id.has_value()) {
            root().for_each_in_inclusive_subtree_of_type<HTMLElement>([&](auto& candidate) {
                if (candidate.attribute(HTML::AttributeNames::id) == target_id.value()) {
                    target = &candidate;
                    return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });
        }
    }

    // 5. If target is not null:
    if (target) {
        // 1. Let command be element's command attribute.
        auto command = this->command();

        // 2. If command is in the Unknown state, then return.
        if (command.is_empty()) {
            return;
        }

        // 3. Let isPopover be true if target's popover attribute is not in the No Popover state; otherwise false.
        auto is_popover = target->popover().has_value();

        // 4. If isPopover is false and command is not in the Custom state:
        auto command_is_in_custom_state = command.starts_with_bytes("--"sv);
        if (!is_popover && !command_is_in_custom_state) {
            // 1. Assert: target's namespace is the HTML namespace.
            VERIFY(target->namespace_uri() == Namespace::HTML);

            // 2. If this standard does not define is valid invoker command steps for target's local name, then return.
            // 3. Otherwise, if the result of running target's corresponding is valid invoker command steps given command is false, then return.
            if (!target->is_valid_invoker_command(command))
                return;
        }

        // 5. Let continue be the result of firing an event named command at target, using CommandEvent, with its
        //    command attribute initialized to command, its source attribute initialized to element, and its cancelable
        //    and composed attributes initialized to true.
        // NOTE: DOM standard issue #1328 tracks how to better standardize associated event data in a way which makes
        //       sense on Events. Currently an event attribute initialized to a value cannot also have a getter, and so
        //       an internal slot (or map of additional fields) is required to properly specify this.
        CommandEventInit event_init {};
        event_init.command = command;
        event_init.source = this;
        event_init.cancelable = true;
        event_init.composed = true;

        auto event = CommandEvent::create(realm(), HTML::EventNames::command, move(event_init));
        event->set_is_trusted(true);
        auto continue_ = target->dispatch_event(event);

        // 6. If continue is false, then return.
        if (!continue_)
            return;

        // 7. If target is not connected, then return.
        if (!target->is_connected())
            return;

        // 8. If command is in the Custom state, then return.
        if (command_is_in_custom_state)
            return;

        // 9. If command is in the Hide Popover state:
        if (command == "hide-popover") {
            // 1. If the result of running check popover validity given target, true, false, and null is true,
            //    then run the hide popover algorithm given target, true, true, false, and element.
            if (MUST(target->check_popover_validity(ExpectedToBeShowing::Yes, ThrowExceptions::No, nullptr, IgnoreDomState::No))) {
                MUST(target->hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::No, this));
            }
        }

        // 10. Otherwise, if command is in the Toggle Popover state:
        else if (command == "toggle-popover") {
            // 1. If the result of running check popover validity given target, false, false, and null is true,
            //    then run the show popover algorithm given target, false, and this.
            if (MUST(target->check_popover_validity(ExpectedToBeShowing::No, ThrowExceptions::No, nullptr, IgnoreDomState::No))) {
                MUST(target->show_popover(ThrowExceptions::No, this));
            }

            // 2. Otherwise, if the result of running check popover validity given target, true, false, and null is true,
            //    then run the hide popover algorithm given target, true, true, false and element.
            else if (MUST(target->check_popover_validity(ExpectedToBeShowing::Yes, ThrowExceptions::No, nullptr, IgnoreDomState::No))) {
                MUST(target->hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::No, this));
            }
        }

        // 11. Otherwise, if command is in the Show Popover state:
        else if (command == "show-popover") {
            // 1. If the result of running check popover validity given target, false, false, and null is true,
            //    then run the show popover algorithm given target, false, and this.
            if (MUST(target->check_popover_validity(ExpectedToBeShowing::No, ThrowExceptions::No, nullptr, IgnoreDomState::No))) {
                MUST(target->show_popover(ThrowExceptions::No, this));
            }
        }

        // 12. Otherwise, if this standard defines invoker command steps for target's local name,
        //     then run the corresponding invoker command steps given target, element, and command.
        else {
            target->invoker_command_steps(*this, command);
        }
    }

    // 6. Otherwise, run the popover target attribute activation behavior given element and event's target.
    else if (event.target() && event.target()->is_dom_node())
        PopoverInvokerElement::popover_target_activation_behaviour(*this, as<DOM::Node>(*event.target()));
}

bool HTMLButtonElement::is_focusable() const
{
    return enabled();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-button-command
String HTMLButtonElement::command() const
{
    // 1. Let command be this's command attribute.
    auto command = get_attribute(AttributeNames::command);

    // https://html.spec.whatwg.org/multipage/form-elements.html#attr-button-command
    // The command attribute is an enumerated attribute with the following keywords and states:
    // Keyword                  State          Brief description
    // toggle-popover           Toggle Popover Shows or hides the targeted popover element.
    // show-popover             Show Popover   Shows the targeted popover element.
    // hide-popover             Hide Popover   Hides the targeted popover element.
    // close                    Close          Closes the targeted dialog element.
    // request-close            Request Close  Requests to close the targeted dialog element.
    // show-modal               Show Modal     Opens the targeted dialog element as modal.
    // A custom command keyword Custom         Only dispatches the command event on the targeted element.
    Array valid_values { "toggle-popover"_string, "show-popover"_string, "hide-popover"_string, "close"_string, "request-close"_string, "show-modal"_string };

    // 2. If command is in the Custom state, then return command's value.
    //    A custom command keyword is a string that starts with "--".
    if (command.has_value() && command.value().starts_with_bytes("--"sv)) {
        return command.value();
    }

    // NOTE: Steps are re-ordered a bit.

    // 4. Return the keyword corresponding to the value of command.return
    if (command.has_value()) {
        auto command_value = command.value();
        for (auto const& value : valid_values) {
            if (value.equals_ignoring_ascii_case(command_value)) {
                return value;
            }
        }
    }

    // 3. If command is in the Unknown state, then return the empty string.
    //    The attribute's missing value default and invalid value default are both the Unknown state.
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:dom-button-command-2
void HTMLButtonElement::set_command(String const& value)
{
    set_attribute_value(AttributeNames::command, value);
}

}
