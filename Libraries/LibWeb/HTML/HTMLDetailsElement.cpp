/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLDetailsElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLSummaryElement.h>
#include <LibWeb/HTML/ToggleEvent.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLDetailsElement);

HTMLDetailsElement::HTMLDetailsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLDetailsElement::~HTMLDetailsElement() = default;

void HTMLDetailsElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_summary_slot);
    visitor.visit(m_descendants_slot);
}

void HTMLDetailsElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLDetailsElement);
}

void HTMLDetailsElement::inserted()
{
    create_shadow_tree_if_needed().release_value_but_fixme_should_propagate_errors();
    update_shadow_tree_slots();
}

void HTMLDetailsElement::removed_from(DOM::Node*)
{
    set_shadow_root(nullptr);
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#the-details-element:concept-element-attributes-change-ext
void HTMLDetailsElement::attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(local_name, old_value, value, namespace_);

    // 1. If namespace is not null, then return.
    if (namespace_.has_value())
        return;

    // 2. If localName is name, then ensure details exclusivity by closing the given element if needed given element.
    if (local_name == HTML::AttributeNames::name) {
        // FIXME: Implement the exclusivity steps.
    }

    // 3. If localName is open, then:
    else if (local_name == HTML::AttributeNames::open) {
        // 1. If one of oldValue or value is null and the other is not null, run the following steps, which are known as
        //    the details notification task steps, for this details element:
        {
            // 1. If oldValue is null, queue a details toggle event task given the details element, "closed", and "open".
            if (!old_value.has_value()) {
                queue_a_details_toggle_event_task("closed"_string, "open"_string);
            }
            // 2. Otherwise, queue a details toggle event task given the details element, "open", and "closed".
            else {
                queue_a_details_toggle_event_task("open"_string, "closed"_string);
            }
        }

        // 2. If oldValue is null and value is not null, then ensure details exclusivity by closing other elements if
        //    needed given element.
        if (!old_value.has_value() && value.has_value()) {
            // FIXME: Implement the exclusivity steps.
        }

        update_shadow_tree_style();
    }
}

void HTMLDetailsElement::children_changed()
{
    Base::children_changed();
    update_shadow_tree_slots();
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#queue-a-details-toggle-event-task
void HTMLDetailsElement::queue_a_details_toggle_event_task(String old_state, String new_state)
{
    // 1. If element's details toggle task tracker is not null, then:
    if (m_details_toggle_task_tracker.has_value()) {
        // 1. Set oldState to element's details toggle task tracker's old state.
        old_state = move(m_details_toggle_task_tracker->old_state);

        // 2. Remove element's details toggle task tracker's task from its task queue.
        HTML::main_thread_event_loop().task_queue().remove_tasks_matching([&](auto const& task) {
            return task.id() == m_details_toggle_task_tracker->task_id;
        });

        // 3. Set element's details toggle task tracker to null.
        m_details_toggle_task_tracker->task_id = {};
    }

    // 2. Queue an element task given the DOM manipulation task source and element to run the following steps:
    auto task_id = queue_an_element_task(HTML::Task::Source::DOMManipulation, [this, old_state, new_state = move(new_state)]() mutable {
        // 1. Fire an event named toggle at element, using ToggleEvent, with the oldState attribute initialized to
        //    oldState and the newState attribute initialized to newState.
        ToggleEventInit event_init {};
        event_init.old_state = move(old_state);
        event_init.new_state = move(new_state);

        dispatch_event(ToggleEvent::create(realm(), HTML::EventNames::toggle, move(event_init)));

        // 2. Set element's details toggle task tracker to null.
        m_details_toggle_task_tracker = {};
    });

    // 3. Set element's details toggle task tracker to a struct with task set to the just-queued task and old state set to oldState.
    m_details_toggle_task_tracker = ToggleTaskTracker {
        .task_id = task_id,
        .old_state = move(old_state),
    };
}

// https://html.spec.whatwg.org/#the-details-and-summary-elements
WebIDL::ExceptionOr<void> HTMLDetailsElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return {};

    auto& realm = this->realm();

    // The details element is expected to have an internal shadow tree with three child elements:
    auto shadow_root = realm.create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    shadow_root->set_slot_assignment(Bindings::SlotAssignmentMode::Manual);

    // The first child element is a slot that is expected to take the details element's first summary element child, if any.
    auto summary_slot = TRY(DOM::create_element(document(), HTML::TagNames::slot, Namespace::HTML));
    MUST(shadow_root->append_child(summary_slot));

    // The second child element is a slot that is expected to take the details element's remaining descendants, if any.
    auto descendants_slot = TRY(DOM::create_element(document(), HTML::TagNames::slot, Namespace::HTML));
    descendants_slot->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::DetailsContent);
    MUST(shadow_root->append_child(descendants_slot));

    // The third child element is either a link or style element with the following styles for the default summary:
    auto style = TRY(DOM::create_element(document(), HTML::TagNames::style, Namespace::HTML));
    style->set_text_content(R"~~~(
        :host summary {
            display: list-item;
            counter-increment: list-item 0;
            list-style: disclosure-closed inside;
        }
        :host([open]) summary {
            list-style-type: disclosure-open;
        }
    )~~~"_string);
    MUST(shadow_root->append_child(style));

    m_summary_slot = static_cast<HTML::HTMLSlotElement&>(*summary_slot);
    m_descendants_slot = static_cast<HTML::HTMLSlotElement&>(*descendants_slot);
    set_shadow_root(shadow_root);

    return {};
}

void HTMLDetailsElement::update_shadow_tree_slots()
{
    if (!shadow_root())
        return;

    Vector<HTMLSlotElement::SlottableHandle> summary_assignment;
    Vector<HTMLSlotElement::SlottableHandle> descendants_assignment;

    auto* summary = first_child_of_type<HTMLSummaryElement>();
    if (summary != nullptr)
        summary_assignment.append(GC::make_root(static_cast<DOM::Element&>(*summary)));

    for_each_in_subtree([&](auto& child) {
        if (&child == summary)
            return TraversalDecision::Continue;
        if (!child.is_slottable())
            return TraversalDecision::Continue;

        child.as_slottable().visit([&](auto& node) {
            descendants_assignment.append(GC::make_root(node));
        });

        return TraversalDecision::Continue;
    });

    m_summary_slot->assign(move(summary_assignment));
    m_descendants_slot->assign(move(descendants_assignment));

    update_shadow_tree_style();
}

// https://html.spec.whatwg.org/#the-details-and-summary-elements:the-details-element-6
void HTMLDetailsElement::update_shadow_tree_style()
{
    if (!shadow_root())
        return;

    if (has_attribute(HTML::AttributeNames::open)) {
        MUST(m_descendants_slot->set_attribute(HTML::AttributeNames::style, R"~~~(
            display: block;
        )~~~"_string));
    } else {
        MUST(m_descendants_slot->set_attribute(HTML::AttributeNames::style, R"~~~(
            display: block;
            content-visibility: hidden;
        )~~~"_string));
    }
}

}
