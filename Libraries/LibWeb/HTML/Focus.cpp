/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/UIEvents/FocusEvent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#fire-a-focus-event
static void fire_a_focus_event(GC::Ptr<DOM::EventTarget> focus_event_target, GC::Ptr<DOM::EventTarget> related_focus_target, FlyString const& event_name, bool bubbles)
{
    // To fire a focus event named e at an element t with a given related target r, fire an event named e at t, using FocusEvent,
    // with the relatedTarget attribute initialized to r, the view attribute initialized to t's node document's relevant global
    // object, and the composed flag set.
    Bindings::FocusEventInit focus_event_init {};
    focus_event_init.related_target = related_focus_target;
    focus_event_init.view = as<HTML::Window>(focus_event_target->realm().global_object()).window();

    auto focus_event = UIEvents::FocusEvent::create(focus_event_target->realm(), event_name, focus_event_init);
    // AD-HOC: support bubbling focus events, used for focusin & focusout.
    //         See: https://github.com/whatwg/html/issues/3514
    focus_event->set_bubbles(bubbles);
    focus_event->set_composed(true);
    focus_event_target->dispatch_event(focus_event);
}

static void designate_document_viewport_as_focused_area(DOM::Document& document)
{
    if (document.focused_area() == nullptr)
        return;

    as<Window>(relevant_global_object(document)).navigation()->set_focus_changed_during_ongoing_navigation(true);

    // AD-HOC: null focused_area indicates "viewport focus".
    document.set_focused_area(nullptr);
}

static bool is_top_level_document_viewport(DOM::Node const* node)
{
    auto const* document = as_if<DOM::Document>(node);
    return document && document->navigable() && !document->navigable()->parent();
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-update-steps
static void run_focus_update_steps(Vector<GC::Root<DOM::Node>> old_chain, Vector<GC::Root<DOM::Node>> new_chain, DOM::Node* new_focus_target)
{
    // The focus update steps, given an old chain, a new chain, and a new focus target respectively, are as follows:

    // 1. If the last entry in old chain and the last entry in new chain are the same, pop the last entry from old chain
    //    and the last entry from new chain and redo this step.
    while (!old_chain.is_empty()
        && !new_chain.is_empty()
        && old_chain.last() == new_chain.last()) {
        (void)old_chain.take_last();
        (void)new_chain.take_last();
    }

    auto new_focus_target_is_document_viewport = new_focus_target && new_focus_target->is_document();

    // 2. For each entry entry in old chain, in order, run these substeps:
    for (auto& entry : old_chain) {
        // 1. If entry is an input element
        if (auto* input_element = as_if<HTMLInputElement>(*entry)) {
            // FIXME: Spec issue: It doesn't make sense to check if the element has a defined activation behavior, as
            //        that is always true. Instead, we check if it has an *input* activation behavior.
            //        https://github.com/whatwg/html/issues/9973
            // and the change event applies to the element, and the element does not have a defined activation behavior,
            // and the user has changed the element's value or its list of selected files while the control was focused
            // without committing that change (such that it is different to what it was when the control was first
            // focused), then:
            if (input_element->change_event_applies() && !input_element->has_input_activation_behavior()
                && input_element->has_uncommitted_changes()) {
                // 1. Set entry's user validity to true.
                input_element->set_user_validity(true);

                // 2. Fire an event named change at the element, with the bubbles attribute initialized to true.
                input_element->commit_pending_changes();
            }
        }

        // 2. If entry is an element, let blur event target be entry.
        //    If entry is a Document object, let blur event target be that Document object's relevant global object.
        //    Otherwise, let blur event target be null.
        GC::Ptr<DOM::EventTarget> blur_event_target;
        if (is<DOM::Element>(*entry))
            blur_event_target = entry;
        else if (is<DOM::Document>(*entry))
            blur_event_target = as<Window>(relevant_global_object(*entry));

        // 3. If entry is the last entry in old chain, and entry is an Element, and the last entry in new chain is also
        //    an Element, then let related blur target be the last entry in new chain. Otherwise, let related blur
        //    target be null.
        GC::Ptr<DOM::EventTarget> related_blur_target;
        if (!old_chain.is_empty()
            && &entry == &old_chain.last()
            && is<DOM::Element>(*entry)
            && !new_chain.is_empty()
            && is<DOM::Element>(*new_chain.last())) {
            related_blur_target = new_chain.last();
        }

        // 4. If blur event target is not null, fire a focus event named blur at blur event target, with related blur
        //    target as the related target.
        // FIXME: NOTE: In some cases, e.g., if entry is an area element's shape, a scrollable region, or a viewport, no event
        //       is fired.
        if (blur_event_target) {
            fire_a_focus_event(blur_event_target, related_blur_target, HTML::EventNames::blur, false);

            // AD-HOC: dispatch focusout
            fire_a_focus_event(blur_event_target, related_blur_target, HTML::EventNames::focusout, true);
        }
    }

    // FIXME: 3. Apply any relevant platform-specific conventions for focusing new focus target. (For example, some platforms
    //    select the contents of a text control when that control is focused.)
    //
    // If new focus target is the Document's viewport, its surrogate Document entry might have
    // already been popped from new chain as the common focus-chain tail. Still designate the
    // viewport as focused so focusing documentElement clears any previously focused element.
    if (new_focus_target_is_document_viewport)
        designate_document_viewport_as_focused_area(as<DOM::Document>(*new_focus_target));

    // 4. For each entry entry in new chain, in reverse order, run these substeps:
    for (auto& entry : new_chain.in_reverse()) {
        // 1. If entry is a focusable area, and the focused area of the document is not entry:
        if (entry->is_document()) {
            designate_document_viewport_as_focused_area(entry->document());
        } else if (entry->is_focusable() && entry->document().focused_area() != entry.ptr()) {
            as<Window>(relevant_global_object(*entry)).navigation()->set_focus_changed_during_ongoing_navigation(true);

            // 2. Designate entry as the focused area of the document.
            entry->document().set_focused_area(*entry);
        }

        // 2. If entry is an element, let focus event target be entry.
        //    If entry is a Document object, let focus event target be that Document object's relevant global object.
        //    Otherwise, let focus event target be null.
        GC::Ptr<DOM::EventTarget> focus_event_target;
        if (entry.ptr() == new_focus_target && is_top_level_document_viewport(new_focus_target)) {
            // The viewport does not fire a focus event. The Document object is only its surrogate.
        } else if (is<DOM::Document>(*entry)) {
            focus_event_target = as<Window>(relevant_global_object(*entry));
        } else if (is<DOM::Element>(*entry)) {
            focus_event_target = *entry;
        }

        // 3. If entry is the last entry in new chain, and entry is an Element, and the last entry in old chain is also
        //    an Element, then let related focus target be the last entry in old chain. Otherwise, let related focus
        //    target be null.
        GC::Ptr<DOM::EventTarget> related_focus_target;
        if (!new_chain.is_empty()
            && &entry == &new_chain.last()
            && is<DOM::Element>(*entry)
            && !old_chain.is_empty()
            && is<DOM::Element>(*old_chain.last())) {
            related_focus_target = old_chain.last();
        }

        // 4. If focus event target is not null, fire a focus event named focus at focus event target, with related
        //    focus target as the related target.
        // FIXME: NOTE: In some cases, e.g. if entry is an area element's shape, a scrollable region, or a viewport, no event
        //       is fired.
        if (focus_event_target) {
            fire_a_focus_event(focus_event_target, related_focus_target, HTML::EventNames::focus, false);

            // AD-HOC: dispatch focusin
            fire_a_focus_event(focus_event_target, related_focus_target, HTML::EventNames::focusin, true);
        }
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-chain
static Vector<GC::Root<DOM::Node>> focus_chain(DOM::Node* subject)
{
    // FIXME: Move this somewhere more spec-friendly.
    if (!subject)
        return {};

    // 1. Let output be an empty list.
    Vector<GC::Root<DOM::Node>> output;

    // 2. Let currentObject be subject.
    auto* current_object = subject;

    // 3. While true:
    while (true) {
        // 1. Append currentObject to output.
        output.append(GC::make_root(*current_object));

        // FIXME: 2. If currentObject is an area element's shape, then append that area element to output.

        // FIXME:    Otherwise, if currentObject's DOM anchor is an element that is not currentObject itself, then append currentObject's DOM anchor to output.

        // FIXME: Everything below needs work. The conditions are not entirely right.
        if (!is<DOM::Document>(*current_object)) {
            // 3. If currentObject is a focusable area, then set currentObject to currentObject's DOM anchor's node document.
            current_object = &current_object->document();
        } else if (is<DOM::Document>(*current_object)
            && current_object->navigable()
            && current_object->navigable()->parent()) {
            // Otherwise, if currentObject is a Document whose node navigable's parent is non-null, then set currentObject to currentObject's node navigable's parent.
            current_object = current_object->navigable()->container();
        } else {
            // Otherwise, break.
            break;
        }
    }

    // 4. Return output.
    return output;
}

static bool document_has_focusable_viewport(DOM::Document& document)
{
    return document.browsing_context() && !document.is_inert();
}

static bool is_inert_for_focus(DOM::Node const& node)
{
    return node.find_in_shadow_including_ancestry([](auto const& ancestor) {
        return ancestor.is_inert();
    });
}

static bool is_focusable_area(DOM::Node& node)
{
    if (node.is_document())
        return document_has_focusable_viewport(node.document());

    if (is_inert_for_focus(node))
        return false;

    if (auto* element = as_if<DOM::Element>(&node); element && element->is_shadow_host() && element->shadow_root()->delegates_focus())
        return false;

    return node.is_focusable();
}

// https://html.spec.whatwg.org/multipage/interaction.html#get-the-focusable-area
static DOM::Node* get_focusable_area(DOM::Node& focus_target, FocusTrigger focus_trigger);

// https://html.spec.whatwg.org/multipage/interaction.html#focus-delegate
static DOM::Node* focus_delegate(DOM::Node& focus_target, FocusTrigger focus_trigger)
{
    auto* where_to_look = &focus_target;

    // 1. If focusTarget is a shadow host and its shadow root's delegates focus is false, then return null.
    // 2. Let whereToLook be focusTarget.
    // 3. If whereToLook is a shadow host, then set whereToLook to whereToLook's shadow root.
    if (auto* element = as_if<DOM::Element>(&focus_target); element && element->is_shadow_host()) {
        auto shadow_root = element->shadow_root();
        if (!shadow_root->delegates_focus())
            return nullptr;
        where_to_look = shadow_root;
    }

    // FIXME: 4. Let autofocusDelegate be the autofocus delegate for whereToLook given focusTrigger.
    // FIXME: 5. If autofocusDelegate is not null, then return autofocusDelegate.

    // 6. For each descendant of whereToLook's descendants, in tree order:
    for (auto* descendant = where_to_look->first_child(); descendant; descendant = descendant->next_in_pre_order(where_to_look)) {
        // FIXME: 1. Let focusableArea be null.
        // FIXME: 2. If focusTarget is a dialog element and descendant is sequentially focusable, then
        //           set focusableArea to descendant.

        // 3. Otherwise, if focusTarget is not a dialog and descendant is a focusable area, set
        //    focusableArea to descendant.
        if (is_focusable_area(*descendant))
            return descendant;

        // 4. Otherwise, set focusableArea to the result of getting the focusable area for descendant
        //    given focusTrigger.
        if (auto* focusable_area = get_focusable_area(*descendant, focus_trigger))
            return focusable_area;
    }

    // 7. Return null.
    return nullptr;
}

static DOM::Node* get_focusable_area(DOM::Node& focus_target, FocusTrigger focus_trigger)
{
    // FIXME: Implement the rest of the get the focusable area algorithm.

    // If focus target is the document element of its Document, return the Document's viewport.
    if (auto* element = as_if<DOM::Element>(&focus_target); element && element == element->document().document_element()) {
        auto& document = element->document();
        if (document_has_focusable_viewport(document))
            return &document;
    }

    // If focus target is a navigable container with a non-null content navigable, return the
    // navigable container's content navigable's active document.
    if (auto* navigable_container = as_if<NavigableContainer>(&focus_target)) {
        if (!is_inert_for_focus(*navigable_container) && navigable_container->meets_focusable_area_rendering_requirements()) {
            if (auto content_navigable = navigable_container->content_navigable())
                return content_navigable->active_document();
        }
    }

    // If focus target is a shadow host whose shadow root's delegates focus is true:
    if (auto* element = as_if<DOM::Element>(&focus_target); element && element->is_shadow_host() && element->shadow_root()->delegates_focus()) {
        if (is_inert_for_focus(*element))
            return nullptr;

        // 1. Let focusedElement be the currently focused area of a top-level traversable's DOM anchor.
        if (auto browsing_context = element->document().browsing_context()) {
            if (auto focused_element = browsing_context->top_level_traversable()->currently_focused_area()) {
                // 2. If focus target is a shadow-including inclusive ancestor of focusedElement, then
                //    return focusedElement.
                if (element->is_shadow_including_inclusive_ancestor_of(*focused_element))
                    return focused_element;
            }
        }

        // 3. Return the focus delegate for focus target given focus trigger.
        return focus_delegate(focus_target, focus_trigger);
    }

    return nullptr;
}

// https://html.spec.whatwg.org/multipage/interaction.html#focusing-steps
// FIXME: This should accept more types.
void run_focusing_steps(DOM::Node* new_focus_target, DOM::Node* fallback_target, FocusTrigger focus_trigger)
{
    // 1. If new focus target is not a focusable area, then set new focus target to the result of getting the focusable
    //    area for new focus target, given focus trigger if it was passed.
    if (new_focus_target && !is_focusable_area(*new_focus_target))
        new_focus_target = get_focusable_area(*new_focus_target, focus_trigger);

    // 2. If new focus target is null, then:
    if (!new_focus_target) {
        // 1. If no fallback target was specified, then return.
        if (!fallback_target)
            return;

        // 2. Otherwise, set new focus target to the fallback target.
        new_focus_target = fallback_target;
    }

    if (!is_focusable_area(*new_focus_target)) {
        if (!fallback_target)
            return;

        new_focus_target = fallback_target;
        if (!is_focusable_area(*new_focus_target))
            return;
    }

    // 3. If new focus target is a navigable container with non-null content navigable, then set new focus target to the content navigable's active document.
    if (auto* navigable_container = as_if<NavigableContainer>(*new_focus_target)) {
        if (auto content_navigable = navigable_container->content_navigable())
            new_focus_target = content_navigable->active_document();
    }

    // 4. If new focus target is a focusable area and its DOM anchor is inert, then return.
    if (is_inert_for_focus(*new_focus_target))
        return;

    // 5. If new focus target is the currently focused area of a top-level browsing context, then return.
    if (!new_focus_target->document().browsing_context())
        return;
    auto top_level_traversable = new_focus_target->document().browsing_context()->top_level_traversable();
    if (new_focus_target == top_level_traversable->currently_focused_area().ptr())
        return;

    // 6. Let old chain be the current focus chain of the top-level browsing context in which
    //    new focus target finds itself.
    auto old_chain = focus_chain(top_level_traversable->currently_focused_area());

    // 7. Let new chain be the focus chain of new focus target.
    auto new_chain = focus_chain(new_focus_target);

    // AD-HOC: Remember last focus trigger for :focus-visible / focus indication.
    new_focus_target->document().set_last_focus_trigger(focus_trigger);

    // 8. Run the focus update steps with old chain, new chain, and new focus target respectively.
    run_focus_update_steps(move(old_chain), move(new_chain), new_focus_target);
}

// https://html.spec.whatwg.org/multipage/interaction.html#unfocusing-steps
void run_unfocusing_steps(DOM::Node* old_focus_target)
{
    // NOTE: The unfocusing steps do not always result in the focus changing, even when applied to the currently focused
    // area of a top-level browsing context. For example, if the currently focused area of a top-level browsing context
    // is a viewport, then it will usually keep its focus regardless until another focusable area is explicitly focused
    // with the focusing steps.

    auto is_shadow_host = [](DOM::Node* node) {
        return is<DOM::Element>(node) && static_cast<DOM::Element*>(node)->is_shadow_host();
    };

    // 1. If old focus target is a shadow host whose shadow root's delegates focus is true, and old focus target's
    //    shadow root is a shadow-including inclusive ancestor of the currently focused area of a top-level browsing
    //    context's DOM anchor, then set old focus target to that currently focused area of a top-level browsing
    //    context.
    if (is_shadow_host(old_focus_target)) {
        auto shadow_root = static_cast<DOM::Element*>(old_focus_target)->shadow_root();
        if (shadow_root->delegates_focus()) {
            auto browsing_context = old_focus_target->document().browsing_context();
            if (!browsing_context)
                return;
            auto top_level_traversable = browsing_context->top_level_traversable();
            if (auto currently_focused_area = top_level_traversable->currently_focused_area()) {
                if (shadow_root->is_shadow_including_ancestor_of(*currently_focused_area)) {
                    old_focus_target = currently_focused_area;
                }
            }
        }
    }

    // 2. If old focus target is inert, then return.
    if (old_focus_target->is_inert())
        return;

    // FIXME: 3. If old focus target is an area element and one of its shapes is the currently focused area of a
    //    top-level browsing context, or, if old focus target is an element with one or more scrollable regions, and one
    //    of them is the currently focused area of a top-level browsing context, then let old focus target be that
    //    currently focused area of a top-level browsing context.

    // NOTE: HTMLAreaElement is currently missing the shapes property

    // 4. Let old chain be the current focus chain of the top-level browsing context in which old focus target finds itself.
    auto browsing_context = old_focus_target->document().browsing_context();
    if (!browsing_context)
        return;
    auto top_level_traversable = browsing_context->top_level_traversable();
    auto currently_focused_area = top_level_traversable->currently_focused_area();
    auto old_chain = focus_chain(currently_focused_area);

    // 5. If old focus target is not one of the entries in old chain, then return.
    auto it = old_chain.find_if([&](auto const& node) { return old_focus_target == node; });
    if (it == old_chain.end())
        return;

    // 6. If old focus target is not a focusable area, then return.
    //
    // The currently focused area, and each Document's designated focused area in its focus chain,
    // remain focused until the focus update steps run, even if they are no longer eligible to be
    // focused again.
    auto old_focus_target_is_designated_focused_area = old_focus_target->document().focused_area().ptr() == old_focus_target;
    if (!old_focus_target_is_designated_focused_area && old_focus_target != currently_focused_area.ptr() && !old_focus_target->is_focusable())
        return;

    // 7. Let topDocument be old chain's last entry.
    auto& top_document = as<DOM::Document>(*old_chain.last());

    // 8. If topDocument's node navigable has system focus, then run the focusing steps for topDocument's viewport.
    if (top_document.navigable()->traversable_navigable()->is_focused()) {

        // AD-HOC: Remove top_document from old_chain so step 1 in run_focus_update_steps doesn't cancel the blur.
        auto without_viewport_surrogate = old_chain;
        without_viewport_surrogate.take_last();

        auto with_viewport_surrogate = focus_chain(&top_document);

        run_focus_update_steps(move(without_viewport_surrogate), move(with_viewport_surrogate), &top_document);
    } else {
        // FIXME: Otherwise, apply any relevant platform-specific conventions for removing system focus from topDocument's
        // browsing context, and run the focus update steps with old chain, an empty list, and null respectively.

        run_focus_update_steps(move(old_chain), {}, nullptr);
    }

    // NOTE: The unfocusing steps do not always result in the focus changing, even when applied to the currently focused
    //       area of a top-level traversable. For example, if the currently focused area of a top-level traversable is a
    //       viewport, then it will usually keep its focus regardless until another focusable area is explicitly focused
    //       with the focusing steps.
}

}
