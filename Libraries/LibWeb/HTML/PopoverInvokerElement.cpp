/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/PopoverInvokerElement.h>

namespace Web::HTML {

void PopoverInvokerElement::associated_attribute_changed(FlyString const& name, Optional<String> const&, Optional<FlyString> const& namespace_)
{
    // From: https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributess
    // For element reflected targets only: the following attribute change steps, given element, localName, oldValue, value, and namespace,
    // are used to synchronize between the content attribute and the IDL attribute:

    // 1. If localName is not attr or namespace is not null, then return.
    if (name != HTML::AttributeNames::popovertarget || namespace_.has_value())
        return;

    // 2. Set element's explicitly set attr-elements to null.
    m_popover_target_element = nullptr;
}

void PopoverInvokerElement::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_popover_target_element);
}

// https://html.spec.whatwg.org/multipage/popover.html#popover-target-attribute-activation-behavior
// https://whatpr.org/html/9457/popover.html#popover-target-attribute-activation-behavior
void PopoverInvokerElement::popover_target_activation_behaviour(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> event_target)
{
    // To run the popover target attribute activation behavior given a Node node and a Node eventTarget:

    // 1. Let popover be node's popover target element.
    auto popover = PopoverInvokerElement::get_the_popover_target_element(node);

    // 2. If popover is null, then return.
    if (!popover)
        return;

    // 3. If eventTarget is a shadow-including inclusive descendant of popover and popover is a shadow-including descendant of node, then return.
    if (event_target->is_shadow_including_inclusive_descendant_of(*popover)
        && popover->is_shadow_including_descendant_of(node))
        return;

    // 4. If node's popovertargetaction attribute is in the show state and popover's popover visibility state is showing, then return.
    if (as<DOM::Element>(*node).get_attribute_value(HTML::AttributeNames::popovertargetaction).equals_ignoring_ascii_case("show"sv)
        && popover->popover_visibility_state() == HTMLElement::PopoverVisibilityState::Showing)
        return;

    // 5. If node's popovertargetaction attribute is in the hide state and popover's popover visibility state is hidden, then return.
    if (as<DOM::Element>(*node).get_attribute_value(HTML::AttributeNames::popovertargetaction).equals_ignoring_ascii_case("hide"sv)
        && popover->popover_visibility_state() == HTMLElement::PopoverVisibilityState::Hidden)
        return;

    // 6. If popover's popover visibility state is showing, then run the hide popover algorithm given popover, true, true, false, and false.
    if (popover->popover_visibility_state() == HTMLElement::PopoverVisibilityState::Showing) {
        MUST(popover->hide_popover(FocusPreviousElement::Yes, FireEvents::Yes, ThrowExceptions::No, IgnoreDomState::No));
    }

    // 7. Otherwise, if popover's popover visibility state is hidden and the result of running check popover validity given popover, false, false, null, and false is true, then run show popover given popover, false, and node.
    else if (popover->popover_visibility_state() == HTMLElement::PopoverVisibilityState::Hidden
        && MUST(popover->check_popover_validity(ExpectedToBeShowing::No, ThrowExceptions::No, nullptr, IgnoreDomState::No))) {
        MUST(popover->show_popover(ThrowExceptions::No, as<HTMLElement>(*node)));
    }
}

// https://html.spec.whatwg.org/multipage/popover.html#popover-target-element
GC::Ptr<HTMLElement> PopoverInvokerElement::get_the_popover_target_element(GC::Ref<DOM::Node> node)
{
    // To get the popover target element given a Node node, perform the following steps. They return an HTML element or null.

    auto const* form_associated_element = dynamic_cast<FormAssociatedElement const*>(node.ptr());
    VERIFY(form_associated_element);

    // 1. If node is not a button, then return null.
    if (!form_associated_element->is_button())
        return {};

    // 2. If node is disabled, then return null.
    if (!form_associated_element->enabled())
        return {};

    // 3. If node has a form owner and node is a submit button, then return null.
    if (form_associated_element->form() != nullptr && form_associated_element->is_submit_button())
        return {};

    // 4. Let popoverElement be the result of running node's get the popovertarget-associated element.
    auto const* popover_invoker_element = dynamic_cast<PopoverInvokerElement const*>(node.ptr());
    VERIFY(popover_invoker_element);
    GC::Ptr<HTMLElement> popover_element = as<HTMLElement>(popover_invoker_element->m_popover_target_element.ptr());
    if (!popover_element) {
        auto target_id = as<HTMLElement>(*node).attribute("popovertarget"_fly_string);
        if (target_id.has_value()) {
            node->root().for_each_in_inclusive_subtree_of_type<HTMLElement>([&](auto& candidate) {
                if (candidate.attribute(HTML::AttributeNames::id) == target_id.value()) {
                    popover_element = &candidate;
                    return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });
        }
    }

    // 5. If popoverElement is null, then return null.
    if (!popover_element)
        return {};

    // 6. If popoverElement's popover attribute is in the no popover state, then return null.
    if (!popover_element->popover().has_value())
        return {};

    // 7. Return popoverElement.
    return popover_element;
}

}
