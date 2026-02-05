/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/Bindings/HTMLLabelElementPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLLabelElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLLabelElement);

HTMLLabelElement::HTMLLabelElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLLabelElement::~HTMLLabelElement() = default;

void HTMLLabelElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLLabelElement);
    Base::initialize(realm);
}

bool HTMLLabelElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/forms.html#the-label-element:activation-behaviour
void HTMLLabelElement::activation_behavior(DOM::Event const& event)
{
    // The label element's exact default presentation and behavior, in particular what its activation behavior might be,
    // if anything, should match the platform's label behavior. The activation behavior of a label element for events
    // targeted at interactive content descendants of a label element, and any descendants of those interactive content
    // descendants, must be to do nothing.

    // AD-HOC: Click and focus the control, matching typical platform behavior.
    //         This matches the behavior of HTMLElement::click(), but the original event properties are preserved.
    if (m_click_in_progress)
        return;

    auto control_element = control();
    if (!control_element)
        return;

    if (auto* form_control = as_if<FormAssociatedElement>(*control_element)) {
        if (!form_control->enabled())
            return;
    }

    {
        m_click_in_progress = true;
        ScopeGuard guard { [this] { m_click_in_progress = false; } };

        auto const& mouse_event = as<UIEvents::MouseEvent>(event);
        auto click_event = mouse_event.clone();

        // Recompute offsetX/offsetY relative to the control element, since the original values are relative to the label.
        if (auto const* paintable = control_element->paintable(); paintable && document().navigable()) {
            auto scroll_offset = document().navigable()->viewport_scroll_offset();
            auto page_position = CSSPixelPoint { CSSPixels(mouse_event.client_x()) + scroll_offset.x(), CSSPixels(mouse_event.client_y()) + scroll_offset.y() };
            auto box_position = paintable->box_type_agnostic_position();
            click_event->set_offset_x(AK::round((page_position.x() - box_position.x()).to_double()));
            click_event->set_offset_y(AK::round((page_position.y() - box_position.y()).to_double()));
        }

        click_event->set_bubbles(true);
        click_event->set_cancelable(true);
        click_event->set_composed(true);
        click_event->set_is_trusted(event.is_trusted());
        control_element->dispatch_event(click_event);
    }

    if (control_element->is_focusable())
        HTML::run_focusing_steps(control_element);
}

// https://html.spec.whatwg.org/multipage/forms.html#labeled-control
GC::Ptr<HTMLElement> HTMLLabelElement::control() const
{
    GC::Ptr<HTMLElement> control;

    // The for attribute may be specified to indicate a form control with which the caption is
    // to be associated. If the attribute is specified, the attribute's value must be the ID of
    // a labelable element in the same tree as the label element. If the attribute is specified
    // and there is an element in the tree whose ID is equal to the value of the for attribute,
    // and the first such element in tree order is a labelable element, then that element is the
    // label element's labeled control.
    if (for_().has_value()) {
        root().for_each_in_inclusive_subtree_of_type<HTMLElement>([&](auto& element) {
            if (element.id() == *for_() && element.is_labelable()) {
                control = &const_cast<HTMLElement&>(element);
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        return control;
    }

    // If the for attribute is not specified, but the label element has a labelable element descendant,
    // then the first such descendant in tree order is the label element's labeled control.
    for_each_in_subtree_of_type<HTMLElement>([&](auto& element) {
        if (element.is_labelable()) {
            control = &const_cast<HTMLElement&>(element);
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });

    return control;
}

// https://html.spec.whatwg.org/multipage/forms.html#dom-label-form
GC::Ptr<HTMLFormElement> HTMLLabelElement::form() const
{
    auto labeled_control = control();

    // 1. If the label element has no labeled control, then return null.
    if (!labeled_control)
        return {};

    // 2. If the label element's labeled control is not a form-associated element, then return null.
    auto* form_associated_element = as_if<FormAssociatedElement>(*labeled_control);
    if (!form_associated_element)
        return {};

    // 3. Return the label element's labeled control's form owner (which can still be null).
    return form_associated_element->form();
}

}
