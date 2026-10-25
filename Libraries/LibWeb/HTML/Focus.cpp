/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/CSS.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Focus.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/RemoteNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/UIEvents/FocusEvent.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#fire-a-focus-event
static void fire_a_focus_event(GC::Ptr<DOM::EventTarget> focus_event_target, GC::Ptr<DOM::EventTarget> related_focus_target, Utf16FlyString const& event_name, bool bubbles)
{
    // To fire a focus event named e at an element t with a given related target r, fire an event named e at t, using FocusEvent,
    // with the relatedTarget attribute initialized to r, the view attribute initialized to t's node document's relevant global
    // object, and the composed flag set.
    UIEvents::FocusEventInit focus_event_init {};
    focus_event_init.related_target = related_focus_target;

    auto& realm = [&]() -> JS::Realm& {
        if (auto* node = as_if<DOM::Node>(focus_event_target.ptr()))
            return node->document().relevant_settings_object().realm();
        auto* window = as_if<Window>(focus_event_target.ptr());
        VERIFY(window);
        return window->principal_realm();
    }();

    auto& global_object = realm.global_object();
    focus_event_init.view = relevant_window(global_object).window();

    auto focus_event_name = event_name.to_utf16_string().to_utf8();
    auto focus_event = UIEvents::FocusEvent::create(FlyString { focus_event_name }, focus_event_init, HighResolutionTime::current_high_resolution_time(global_object));
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

    relevant_window(document).navigation()->set_focus_changed_during_ongoing_navigation(true);

    // AD-HOC: null focused_area indicates "viewport focus".
    document.set_focused_area(nullptr);
}

// The top-level traversable whose currently focused area the focus algorithms compare against.
static GC::Ptr<Navigable> focus_root(DOM::Document& document)
{
    if (!document.browsing_context())
        return nullptr;
    auto navigable = document.navigable();
    if (!navigable)
        return nullptr;
    return navigable->top_level_traversable();
}

static GC::Ptr<Navigable> navigable_of_focused_area(DOM::Node& focused_area)
{
    if (auto* container = as_if<NavigableContainer>(focused_area)) {
        if (auto content_navigable = container->content_navigable(); content_navigable && !is<LocalNavigable>(*content_navigable))
            return content_navigable;
    }
    return focused_area.document().navigable();
}

static bool is_top_level_document_viewport(DOM::Node const* node)
{
    auto const* document = as_if<DOM::Document>(node);
    return document && document->navigable() && !document->navigable()->parent();
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-update-steps
void run_focus_update_steps(Vector<GC::Root<DOM::Node>> old_chain, Vector<GC::Root<DOM::Node>> new_chain, GC::Ptr<DOM::Node> new_focus_target)
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
    GC::Ptr<DOM::Node> focus_transition_source;
    Vector<GC::Root<DOM::Element>> old_focus_within_chain;

    auto build_focus_within_chain = [](GC::Ptr<DOM::Node> source) {
        Vector<GC::Root<DOM::Element>> chain;
        for (auto* node = source.ptr(); node; node = node->flat_tree_parent()) {
            if (auto* element = as_if<DOM::Element>(*node))
                chain.append(*element);
        }
        return chain;
    };

    auto finish_focus_transition = [&](GC::Ptr<DOM::Node> destination) {
        if (!focus_transition_source)
            return;

        CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::Focus, focus_transition_source, destination);
        CSS::Invalidation::invalidate_style_after_pseudo_class_state_change(CSS::PseudoClass::FocusVisible, focus_transition_source, destination);

        auto new_focus_within_chain = build_focus_within_chain(destination);
        for (auto& element : old_focus_within_chain) {
            if (!new_focus_within_chain.contains_slow(element))
                CSS::record_element_state_changed(*element, CSS::PseudoClass::FocusWithin, false);
        }
        for (auto& element : new_focus_within_chain) {
            if (!old_focus_within_chain.contains_slow(element))
                CSS::record_element_state_changed(*element, CSS::PseudoClass::FocusWithin, true);
        }

        focus_transition_source = nullptr;
        old_focus_within_chain.clear();
    };

    // AD-HOC: A blur or focus handler can move focus, re-entering these steps. The spec algorithm
    //         has no answer for that: the old area stays designated while its blur fires, so a
    //         handler that calls focus() derives its old chain from the same designated area and
    //         blurs it again, without bound. Like other engines, clear each entry's designation
    //         before its blur event fires, and stop this update when a handler moved focus: the
    //         nested update has already designated and fired events for the new area.
    auto currently_focused_area = [&]() -> GC::Ptr<DOM::Node> {
        for (auto const* chain : { &old_chain, &new_chain }) {
            for (auto const& entry : *chain) {
                if (auto root = focus_root(entry->document()))
                    return root->currently_focused_area();
            }
        }
        return nullptr;
    };

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
            blur_event_target = &relevant_window(*entry);

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

        // AD-HOC: Clear the entry's designation before its blur event fires; see above. Keep the CSS
        //         focus-state transition pending until the new entry has passed its focusability
        //         check. The specification leaves the old entry designated during this check, so
        //         publishing a transition through null here could make the new entry stop rendering.
        //         Snapshot the :focus-within chain because a blur handler can detach the old entry.
        if (auto* element = as_if<DOM::Element>(*entry); element && element->document().focused_area().ptr() == element) {
            auto preserve_focus_state = new_focus_target && !new_focus_target->is_document() && &new_focus_target->document() == &element->document();
            if (preserve_focus_state) {
                focus_transition_source = element;
                old_focus_within_chain = build_focus_within_chain(element);
            }
            element->document().set_focused_area(nullptr, preserve_focus_state ? DOM::Document::InvalidateFocusPseudoClasses::No : DOM::Document::InvalidateFocusPseudoClasses::Yes);
        }

        // 4. If blur event target is not null, fire a focus event named blur at blur event target, with related blur
        //    target as the related target.
        // FIXME: NOTE: In some cases, e.g., if entry is an area element's shape, a scrollable region, or a viewport, no event
        //       is fired.
        if (blur_event_target) {
            auto focused_before_dispatch = currently_focused_area();
            fire_a_focus_event(blur_event_target, related_blur_target, HTML::EventNames::blur, false);

            // AD-HOC: dispatch focusout
            fire_a_focus_event(blur_event_target, related_blur_target, HTML::EventNames::focusout, true);

            // AD-HOC: A handler moved focus; the nested update owns the designation. See above.
            if (currently_focused_area() != focused_before_dispatch) {
                auto destination = focus_transition_source ? focus_transition_source->document().focused_area() : nullptr;
                finish_focus_transition(destination);
                return;
            }
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
        } else if (entry->document().focused_area().ptr() != entry.ptr()) {
            if (entry->is_focusable()) {
                relevant_window(*entry).navigation()->set_focus_changed_during_ongoing_navigation(true);

                // 2. Designate entry as the focused area of the document.
                auto had_focus_transition = focus_transition_source != nullptr;
                finish_focus_transition(new_focus_target);
                entry->document().set_focused_area(*entry, had_focus_transition ? DOM::Document::InvalidateFocusPseudoClasses::No : DOM::Document::InvalidateFocusPseudoClasses::Yes);
            } else {
                finish_focus_transition(nullptr);
            }
        }

        // 2. If entry is an element, let focus event target be entry.
        //    If entry is a Document object, let focus event target be that Document object's relevant global object.
        //    Otherwise, let focus event target be null.
        GC::Ptr<DOM::EventTarget> focus_event_target;
        if (entry.ptr() == new_focus_target.ptr() && is_top_level_document_viewport(new_focus_target.ptr())) {
            // The viewport does not fire a focus event. The Document object is only its surrogate.
        } else if (is<DOM::Document>(*entry)) {
            focus_event_target = &relevant_window(*entry);
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
            auto focused_before_dispatch = currently_focused_area();
            fire_a_focus_event(focus_event_target, related_focus_target, HTML::EventNames::focus, false);

            // AD-HOC: A focus handler moved focus; the nested update owns the designation and has fired the
            //         events for the new area. See above. Stop before the focusin too: a focusin at the entry
            //         focus just left, with the newly focused element as its relatedTarget, tells a focusin
            //         listener that focus arrived here from there, when it went the other way. The spec's steps
            //         don't account for a handler moving focus at all:
            //         https://github.com/whatwg/html/issues/12329
            if (currently_focused_area() != focused_before_dispatch)
                return;

            // AD-HOC: dispatch focusin
            fire_a_focus_event(focus_event_target, related_focus_target, HTML::EventNames::focusin, true);

            // AD-HOC: A handler moved focus; the nested update owns the designation. See above.
            if (currently_focused_area() != focused_before_dispatch)
                return;
        }
    }

    finish_focus_transition(new_focus_target);
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-chain
Vector<GC::Root<DOM::Node>> focus_chain(GC::Ptr<DOM::Node> subject)
{
    // FIXME: Move this somewhere more spec-friendly.
    if (!subject)
        return {};

    // 1. Let output be an empty list.
    Vector<GC::Root<DOM::Node>> output;

    // 2. Let currentObject be subject.
    auto current_object = subject;

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
            // NB: A container in another process ends the part of the chain this process holds.
            auto container = current_object->navigable()->container();
            if (!container)
                break;
            current_object = container;
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
    // The shapes of area elements in an image map have their associated img element as their DOM anchor, so inertness
    // applies to the image rather than the area element. HTMLAreaElement::is_focusable() accounts for the inertness of
    // the associated image.
    if (is<HTMLAreaElement>(node))
        return false;

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
static GC::Ptr<DOM::Node> get_focusable_area(DOM::Node& focus_target, FocusTrigger focus_trigger);

static bool is_sequentially_focusable(DOM::Node const&);

// https://html.spec.whatwg.org/multipage/interaction.html#autofocus-delegate
static GC::Ptr<DOM::Node> autofocus_delegate(DOM::Node& focus_target, FocusTrigger focus_trigger)
{
    // 1. For each descendant descendant of focus target, in tree order:
    for (auto* descendant = focus_target.first_child(); descendant; descendant = descendant->next_in_pre_order(&focus_target)) {
        // 1. If descendant does not have an autofocus content attribute, then continue.
        auto* element = as_if<DOM::Element>(*descendant);
        if (!element || !element->has_attribute(AttributeNames::autofocus))
            continue;

        // 2. Let focusable area be descendant, if descendant is a focusable area; otherwise let focusable area be the
        //    result of getting the focusable area for descendant given focus trigger.
        GC::Ptr<DOM::Node> focusable_area = is_focusable_area(*descendant) ? descendant : get_focusable_area(*descendant, focus_trigger);

        // 3. If focusable area is null, then continue.
        if (!focusable_area)
            continue;

        // FIXME: 4. If focusable area is not click focusable and focus trigger is "click", then continue.

        // 5. Return focusable area.
        return focusable_area;
    }

    // 2. Return null.
    return nullptr;
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-delegate
GC::Ptr<DOM::Node> focus_delegate(DOM::Node& focus_target, FocusTrigger focus_trigger)
{
    GC::Ptr<DOM::Node> where_to_look = focus_target;

    // 1. If focusTarget is a shadow host and its shadow root's delegates focus is false, then return null.
    // 2. Let whereToLook be focusTarget.
    // 3. If whereToLook is a shadow host, then set whereToLook to whereToLook's shadow root.
    if (auto* element = as_if<DOM::Element>(&focus_target); element && element->is_shadow_host()) {
        auto shadow_root = element->shadow_root();
        if (!shadow_root->delegates_focus())
            return nullptr;
        where_to_look = shadow_root;
    }

    // 4. Let autofocusDelegate be the autofocus delegate for whereToLook given focusTrigger.
    auto autofocus_delegate = HTML::autofocus_delegate(*where_to_look, focus_trigger);

    // 5. If autofocusDelegate is not null, then return autofocusDelegate.
    if (autofocus_delegate)
        return autofocus_delegate;

    // 6. For each descendant of whereToLook's descendants, in tree order:
    for (auto* descendant = where_to_look->first_child(); descendant; descendant = descendant->next_in_pre_order(where_to_look.ptr())) {
        // 1. Let focusableArea be null.
        GC::Ptr<DOM::Node> focusable_area;

        // 2. If focusTarget is a dialog element and descendant is sequentially focusable, then set focusableArea to
        //    descendant.
        if (is<HTMLDialogElement>(focus_target)) {
            if (is_sequentially_focusable(*descendant))
                focusable_area = descendant;
        }
        // 3. Otherwise, if focusTarget is not a dialog and descendant is a focusable area, set focusableArea to
        //    descendant.
        else if (is_focusable_area(*descendant)) {
            focusable_area = descendant;
        }

        // 4. Otherwise, set focusableArea to the result of getting the focusable area for descendant given
        //    focusTrigger.
        if (!focusable_area)
            focusable_area = get_focusable_area(*descendant, focus_trigger);

        // 5. If focusableArea is not null, then return focusableArea.
        if (focusable_area)
            return focusable_area;
    }

    // 7. Return null.
    return nullptr;
}

static GC::Ptr<DOM::Node> get_focusable_area(DOM::Node& focus_target, FocusTrigger focus_trigger)
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
            if (auto content_navigable = navigable_container->content_navigable()) {
                // NB: The active document of a navigable another process hosts is there. The focusing steps focus it
                //     through its container.
                auto* local_navigable = as_if<LocalNavigable>(*content_navigable);
                if (!local_navigable)
                    return navigable_container;
                return local_navigable->active_document();
            }
        }
    }

    // If focus target is a shadow host whose shadow root's delegates focus is true:
    if (auto* element = as_if<DOM::Element>(&focus_target); element && element->is_shadow_host() && element->shadow_root()->delegates_focus()) {
        if (is_inert_for_focus(*element))
            return nullptr;

        // 1. Let focusedElement be the currently focused area of a top-level traversable's DOM anchor.
        if (auto root = focus_root(element->document())) {
            if (auto focused_element = root->currently_focused_area()) {
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
void run_focusing_steps(GC::Ptr<DOM::Node> new_focus_target, GC::Ptr<DOM::Node> fallback_target, FocusTrigger focus_trigger, ScrollIntoView scroll_into_view)
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
    GC::Ptr<RemoteNavigable> remote_content_navigable;
    if (auto* navigable_container = as_if<NavigableContainer>(*new_focus_target)) {
        if (auto content_navigable = navigable_container->content_navigable()) {
            // NB: The active document of a navigable another process hosts is there, and that process runs these steps
            //     for it. The container stands for it in the rest of these steps, as the focused area of its node
            //     document.
            if (auto* local_navigable = as_if<LocalNavigable>(*content_navigable))
                new_focus_target = local_navigable->active_document();
            else
                remote_content_navigable = as<RemoteNavigable>(*content_navigable);
        }
    }

    // 4. If new focus target is a focusable area and its DOM anchor is inert, then return.
    if (is_inert_for_focus(*new_focus_target))
        return;

    if (remote_content_navigable)
        new_focus_target->document().page().client().request_focusing_steps_for_remote_navigable(*remote_content_navigable, focus_trigger);

    // 5. If new focus target is the currently focused area of a top-level browsing context, then return.
    auto root = focus_root(new_focus_target->document());
    if (!root)
        return;
    if (new_focus_target.ptr() == root->currently_focused_area().ptr())
        return;

    // 6. Let old chain be the current focus chain of the top-level browsing context in which
    //    new focus target finds itself.
    auto old_chain = focus_chain(root->currently_focused_area());

    // 7. Let new chain be the focus chain of new focus target.
    auto new_chain = focus_chain(new_focus_target);

    // AD-HOC: Remember last focus trigger for :focus-visible / focus indication.
    new_focus_target->document().set_last_focus_trigger(focus_trigger);

    // NB: The tab's focused navigable shows the walk below a top-level traversable another process hosts, and the other
    //     pages of the tab follow it, so it moves to the navigable of new focus target before any focus event fires.
    if (auto navigable = navigable_of_focused_area(*new_focus_target))
        navigable->page().set_focused_navigable(*navigable);

    // 8. Run the focus update steps with old chain, new chain, and new focus target respectively.
    run_focus_update_steps(move(old_chain), move(new_chain), new_focus_target);

    // INTEROP: Keyboard input follows the deepest focused navigable. Focus event handlers can move focus reentrantly,
    //          so derive the input target from the final focused area instead of the target which began this update.
    auto focused_area = root->currently_focused_area();
    if (focused_area) {
        if (auto navigable = navigable_of_focused_area(*focused_area))
            navigable->page().set_focused_navigable(*navigable);
    }

    // AD-HOC: An element focused by clicking it is already under the pointer, so other engines do not scroll it into
    //         view. Scrolling would also move the element out from under the pointer between mousedown and mouseup,
    //         leaving the two with different hit-test targets and suppressing the click.
    if (focus_trigger == FocusTrigger::Click || scroll_into_view == ScrollIntoView::No)
        return;

    // NB: A focus event handler may have moved focus on to something else. That nested run of these steps is
    //     responsible for revealing whatever it focused, and may have been asked not to reveal it at all, so this run
    //     only reveals the target it focused itself.
    if (focused_area.ptr() != new_focus_target.ptr())
        return;

    // AD-HOC: Scroll the newly focused element into view. The focus update steps do not do this, so an element
    //         focused any way other than by focus() would never be revealed.
    // NB: Not if the focused element is a navigable container: focus moving into a frame lands on its container
    //     element in this document, and other engines do not scroll embedded content into view on focus. Ad scripts
    //     commonly pull focus into an offscreen iframe during page load, and scrolling to it would hijack the
    //     viewport.
    auto* focused_element = as_if<DOM::Element>(focused_area.ptr());
    if (!focused_element || is<NavigableContainer>(*focused_element))
        return;

    Bindings::ScrollIntoViewOptions scroll_options;
    scroll_options.block = Bindings::ScrollLogicalPosition::Nearest;
    scroll_options.inline_ = Bindings::ScrollLogicalPosition::Nearest;
    (void)focused_element->scroll_into_view(scroll_options, {});
}

// https://html.spec.whatwg.org/multipage/interaction.html#unfocusing-steps
void run_unfocusing_steps(GC::Ptr<DOM::Node> old_focus_target)
{
    // NOTE: The unfocusing steps do not always result in the focus changing, even when applied to the currently focused
    // area of a top-level browsing context. For example, if the currently focused area of a top-level browsing context
    // is a viewport, then it will usually keep its focus regardless until another focusable area is explicitly focused
    // with the focusing steps.

    auto is_shadow_host = [](GC::Ptr<DOM::Node> node) {
        return node && is<DOM::Element>(*node) && as<DOM::Element>(*node).is_shadow_host();
    };

    if (!old_focus_target)
        return;

    // 1. If old focus target is a shadow host whose shadow root's delegates focus is true, and old focus target's
    //    shadow root is a shadow-including inclusive ancestor of the currently focused area of a top-level browsing
    //    context's DOM anchor, then set old focus target to that currently focused area of a top-level browsing
    //    context.
    if (is_shadow_host(old_focus_target)) {
        auto shadow_root = as<DOM::Element>(*old_focus_target).shadow_root();
        if (shadow_root->delegates_focus()) {
            auto root = focus_root(old_focus_target->document());
            if (!root)
                return;
            if (auto currently_focused_area = root->currently_focused_area()) {
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
    auto root = focus_root(old_focus_target->document());
    if (!root)
        return;
    auto currently_focused_area = root->currently_focused_area();
    auto old_chain = focus_chain(currently_focused_area);

    // 5. If old focus target is not one of the entries in old chain, then return.
    auto it = old_chain.find_if([&](auto const& node) { return old_focus_target.ptr() == node.ptr(); });
    if (it == old_chain.end())
        return;

    // 6. If old focus target is not a focusable area, then return.
    //
    // The currently focused area, and each Document's designated focused area in its focus chain,
    // remain focused until the focus update steps run, even if they are no longer eligible to be
    // focused again.
    auto old_focus_target_is_designated_focused_area = old_focus_target->document().focused_area().ptr() == old_focus_target.ptr();
    if (!old_focus_target_is_designated_focused_area && old_focus_target != currently_focused_area && !old_focus_target->is_focusable())
        return;

    // 7. Let topDocument be old chain's last entry.
    auto& top_document = as<DOM::Document>(*old_chain.last());

    // 8. If topDocument's node navigable has system focus, then run the focusing steps for topDocument's viewport.
    if (top_document.navigable()->is_focused()) {

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

    // NB: The tab's focused navigable follows the focused area, as in the focusing steps: focus that left a container
    //     another process hosts has left that process.
    GC::Ptr<Navigable> focused_navigable = top_document.navigable();
    if (auto focused_area = root->currently_focused_area())
        focused_navigable = navigable_of_focused_area(*focused_area);
    if (focused_navigable)
        focused_navigable->page().set_focused_navigable(*focused_navigable);

    // NOTE: The unfocusing steps do not always result in the focus changing, even when applied to the currently focused
    //       area of a top-level traversable. For example, if the currently focused area of a top-level traversable is a
    //       viewport, then it will usually keep its focus regardless until another focusable area is explicitly focused
    //       with the focusing steps.
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-navigation-scope-owner
static bool is_focus_navigation_scope_owner(DOM::Node const& node)
{
    // A node is a focus navigation scope owner if it is a Document, a shadow host, a slot, or an element which is the
    // popover trigger of an element in the popover showing state.
    // FIXME: Handle popover triggers.
    if (is<DOM::Document>(node))
        return true;
    if (auto const* element = as_if<DOM::Element>(node))
        return element->is_shadow_host() || is<HTMLSlotElement>(*element);
    return false;
}

// NB: Appends the descendants of parent that have the same associated focus navigation owner as parent's children.
static void append_focus_navigation_scope_descendants(DOM::Node& parent, Vector<GC::Ref<DOM::Element>>& scope)
{
    for (auto* child = parent.first_child(); child; child = child->next_sibling()) {
        auto* element = as_if<DOM::Element>(*child);
        if (!element)
            continue;
        scope.append(*element);

        // NB: The associated focus navigation owner of a shadow host's child is its assigned slot.
        if (element->is_shadow_host())
            continue;

        // INTEROP: A slot's fallback content belongs to the slot's own focus navigation scope; see
        //          focus_navigation_scope().
        if (is<HTMLSlotElement>(*element))
            continue;

        append_focus_navigation_scope_descendants(*element, scope);
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#focus-navigation-scope
static GC::ConservativeVector<GC::Ref<DOM::Element>> focus_navigation_scope(DOM::Node& owner)
{
    // The contents of a given focus navigation scope owner owner's focus navigation scope are all elements whose
    // associated focus navigation owner is owner.
    // NB: We collect them in tree order by walking down from owner, instead of computing the associated focus
    //     navigation owner of every element.
    GC::ConservativeVector<GC::Ref<DOM::Element>> scope;

    if (auto* document = as_if<DOM::Document>(owner)) {
        // NB: We treat the document element as being in its document's scope.
        if (auto* document_element = document->document_element()) {
            scope.append(*document_element);
            append_focus_navigation_scope_descendants(*document_element, scope);
        }
        return scope;
    }

    if (auto* slot = as_if<HTMLSlotElement>(owner)) {
        // INTEROP: The spec puts a slot's fallback content in the focus navigation scope of the slot's shadow host,
        //          ordered by tabindex among the other elements of the shadow tree. Browsers instead put it in the
        //          slot's own scope, so it is navigated at the position of the slot, where it is rendered. We match
        //          them. The fallback content is only rendered while the slot has no assigned nodes.
        if (slot->assigned_nodes_internal().is_empty()) {
            append_focus_navigation_scope_descendants(*slot, scope);
            return scope;
        }

        for (auto const& slottable : slot->assigned_nodes_internal()) {
            auto const* element = slottable.get_pointer<GC::Ref<DOM::Element>>();
            if (!element)
                continue;
            scope.append(*element);
            // NB: A shadow host or a slot owns the focus navigation scope of its own descendants.
            if (!(*element)->is_shadow_host() && !is<HTMLSlotElement>(**element))
                append_focus_navigation_scope_descendants(*element, scope);
        }
        return scope;
    }

    if (auto* element = as_if<DOM::Element>(owner); element && element->is_shadow_host())
        append_focus_navigation_scope_descendants(*element->shadow_root(), scope);

    return scope;
}

// NB: Whether element is a focusable area, for the purposes of sequential focus navigation.
static bool is_sequential_focus_navigation_focusable_area(DOM::Element const& element)
{
    // INTEROP: A slot with a tabindex attribute is a focusable area in the spec, but browsers never focus a slot
    //          through sequential focus navigation. We match them.
    if (is<HTMLSlotElement>(element))
        return false;
    return is_focusable_area(const_cast<DOM::Element&>(element));
}

// https://html.spec.whatwg.org/multipage/interaction.html#tabindex-value
static Optional<i32> tabindex_value(DOM::Element const& element)
{
    auto attribute = element.attribute(AttributeNames::tabindex);
    if (!attribute.has_value())
        return {};
    return parse_integer(*attribute);
}

// https://html.spec.whatwg.org/multipage/interaction.html#sequentially-focusable
static bool is_sequentially_focusable(DOM::Node const& node)
{
    // NB: A focusable area is sequentially focusable unless its tabindex value is a negative integer.
    auto const* element = as_if<DOM::Element>(node);
    if (!element || !is_focusable_area(const_cast<DOM::Element&>(*element)))
        return false;
    return tabindex_value(*element).value_or(0) >= 0;
}

// https://html.spec.whatwg.org/multipage/interaction.html#tabindex-ordered-focus-navigation-scope
static GC::ConservativeVector<GC::Ref<DOM::Element>> tabindex_ordered_focus_navigation_scope(DOM::Node& owner)
{
    // A tabindex-ordered focus navigation scope is a list of focusable areas and focus navigation scope owners.
    // Every focus navigation scope owner owner has tabindex-ordered focus navigation scope, whose contents are
    // determined as follows:
    // - It contains all elements in owner's focus navigation scope that are themselves focus navigation scope owners,
    //   except the elements whose tabindex value is a negative integer.
    // - It contains all of the focusable areas whose DOM anchor is an element in owner's focus navigation scope,
    //   except the focusable areas whose tabindex value is a negative integer.
    // The order within a tabindex-ordered focus navigation scope is determined by each element's tabindex value.
    // NB: Elements with a positive tabindex value come first, in increasing order of that value and then in tree
    //     order. The elements with a tabindex value of zero or null follow in tree order.
    struct Item {
        GC::Ref<DOM::Element> element;
        i32 tabindex;
        size_t tree_index;
    };
    GC::ConservativeVector<Item> positive_items;
    GC::ConservativeVector<GC::Ref<DOM::Element>> result;

    for (auto const& element : focus_navigation_scope(owner)) {
        if (!is_focus_navigation_scope_owner(*element) && !is_sequential_focus_navigation_focusable_area(*element))
            continue;

        auto tabindex = tabindex_value(*element).value_or(0);
        if (tabindex < 0)
            continue;

        if (tabindex > 0)
            positive_items.append({ element, tabindex, positive_items.size() });
        else
            result.append(element);
    }

    // NB: Elements with the same tabindex value stay in tree order.
    quick_sort(positive_items, [](auto const& a, auto const& b) {
        if (a.tabindex != b.tabindex)
            return a.tabindex < b.tabindex;
        return a.tree_index < b.tree_index;
    });

    GC::ConservativeVector<GC::Ref<DOM::Element>> ordered;
    ordered.ensure_capacity(positive_items.size() + result.size());
    for (auto const& item : positive_items)
        ordered.unchecked_append(item.element);
    ordered.extend(move(result));
    return ordered;
}

// https://html.spec.whatwg.org/multipage/interaction.html#flattened-tabindex-ordered-focus-navigation-scope
static void append_flattened_tabindex_ordered_focus_navigation_scope(DOM::Node& owner, Vector<GC::Ref<DOM::Element>>& result)
{
    // 1. Let result be a clone of owner's tabindex-ordered focus navigation scope.
    // 2. For each item of result:
    for (auto const& item : tabindex_ordered_focus_navigation_scope(owner)) {
        // 1. If item is not a focus navigation scope owner, then continue.
        if (!is_focus_navigation_scope_owner(*item)) {
            result.append(item);
            continue;
        }

        // 2. If item is not a focusable area, then replace item with all of the items in item's flattened
        //    tabindex-ordered focus navigation scope.
        if (!is_sequential_focus_navigation_focusable_area(*item)) {
            append_flattened_tabindex_ordered_focus_navigation_scope(*item, result);
            continue;
        }

        // 3. Otherwise, insert the contents of item's flattened tabindex-ordered focus navigation scope after item.
        result.append(item);
        append_flattened_tabindex_ordered_focus_navigation_scope(*item, result);
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#sequential-focus-navigation-order
GC::ConservativeVector<GC::Ref<DOM::Element>> sequential_focus_navigation_order(DOM::Document& document)
{
    // Each Document has a sequential focus navigation order, which orders some or all of the focusable areas in the
    // Document relative to each other. Its contents and ordering are given by the flattened tabindex-ordered focus
    // navigation scope of the Document.
    GC::ConservativeVector<GC::Ref<DOM::Element>> order;
    append_flattened_tabindex_ordered_focus_navigation_scope(document, order);
    return order;
}

// https://html.spec.whatwg.org/multipage/interaction.html#suitable-sequentially-focusable-area
static bool is_suitable_sequentially_focusable_area(DOM::Element const& element)
{
    // A suitable sequentially focusable area is a focusable area whose DOM anchor is not inert and is sequentially
    // focusable.
    if (element.is_inert() || !is_sequential_focus_navigation_focusable_area(element))
        return false;
    return tabindex_value(element).value_or(0) >= 0;
}

// https://html.spec.whatwg.org/multipage/interaction.html#sequential-navigation-search-algorithm
GC::Ptr<DOM::Element> sequential_navigation_search(Variant<GC::Ref<LocalNavigable>, GC::Ref<DOM::Node>> starting_point, SequentialFocusDirection direction, SelectionMechanism selection_mechanism)
{
    // 1. Pick the appropriate cell from the following table, and follow the instructions in that cell.
    //    The appropriate cell is the one that is from the column whose header describes direction and from the first
    //    row whose header describes starting point and selection mechanism.
    GC::Ptr<DOM::Element> candidate;

    if (auto const* navigable = starting_point.get_pointer<GC::Ref<LocalNavigable>>()) {
        // -> starting point is a navigable
        //    forward: Let candidate be the first suitable sequentially focusable area in starting point's active
        //             document, if any; or else null.
        //    backward: Let candidate be the last suitable sequentially focusable area in starting point's active
        //              document, if any; or else null.
        auto document = (*navigable)->active_document();
        if (!document)
            return nullptr;
        auto order = sequential_focus_navigation_order(*document);
        if (direction == SequentialFocusDirection::Forward) {
            for (auto const& element : order) {
                if (is_suitable_sequentially_focusable_area(*element)) {
                    candidate = element;
                    break;
                }
            }
        } else {
            for (auto const& element : order.in_reverse()) {
                if (is_suitable_sequentially_focusable_area(*element)) {
                    candidate = element;
                    break;
                }
            }
        }
    } else {
        auto node = starting_point.get<GC::Ref<DOM::Node>>();

        if (selection_mechanism == SelectionMechanism::DOM) {
            // -> selection mechanism is "DOM"
            //    forward: Let candidate be the suitable sequentially focusable area, that appears nearest after
            //             starting point in starting point's Document, in shadow-including tree order, if any; or
            //             else null.
            //    backward: Let candidate be the suitable sequentially focusable area, that appears nearest before
            //              starting point in starting point's Document, in shadow-including tree order, if any; or
            //              else null.
            bool passed_starting_point = false;
            node->document().for_each_shadow_including_inclusive_descendant([&](DOM::Node& descendant) {
                if (&descendant == node.ptr()) {
                    passed_starting_point = true;
                    return direction == SequentialFocusDirection::Forward ? TraversalDecision::Continue : TraversalDecision::Break;
                }
                auto* element = as_if<DOM::Element>(descendant);
                if (!element || !is_suitable_sequentially_focusable_area(*element))
                    return TraversalDecision::Continue;
                if (direction == SequentialFocusDirection::Backward) {
                    candidate = element;
                    return TraversalDecision::Continue;
                }
                if (passed_starting_point) {
                    candidate = element;
                    return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });
        } else {
            // -> selection mechanism is "sequential"
            //    forward: Let candidate be the first suitable sequentially focusable area after starting point, in
            //             starting point's Document's sequential focus navigation order, if any; or else null.
            //    backward: Let candidate be the last suitable sequentially focusable area before starting point, in
            //              starting point's Document's sequential focus navigation order, if any; or else null.
            auto order = sequential_focus_navigation_order(node->document());
            auto index = order.find_first_index_if([&](auto const& element) { return element.ptr() == node.ptr(); });
            VERIFY(index.has_value());
            if (direction == SequentialFocusDirection::Forward) {
                for (size_t i = *index + 1; i < order.size(); ++i) {
                    if (is_suitable_sequentially_focusable_area(*order[i])) {
                        candidate = order[i];
                        break;
                    }
                }
            } else {
                for (size_t i = *index; i > 0; --i) {
                    if (is_suitable_sequentially_focusable_area(*order[i - 1])) {
                        candidate = order[i - 1];
                        break;
                    }
                }
            }
        }
    }

    // FIXME: 2. If candidate is a navigable container with a non-null content navigable:
    //           1. Let recursive candidate be the result of running the sequential navigation search algorithm with
    //              candidate's content navigable, direction, and "sequential".
    //           2. If recursive candidate is null, then return the result of running the sequential navigation search
    //              algorithm with candidate, direction, and selection mechanism.
    //           3. Otherwise, set candidate to recursive candidate.

    // 3. Return candidate.
    return candidate;
}

}
