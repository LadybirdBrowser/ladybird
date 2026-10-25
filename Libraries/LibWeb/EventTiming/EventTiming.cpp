/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/EventTiming/EventCounts.h>
#include <LibWeb/EventTiming/EventTiming.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/UIEvents/EventNames.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/MouseEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::EventTiming {

WindowEventTimingState::WindowEventTimingState()
    // NB: The user interaction value is initially set to a random integer between 100 and 10000.
    : user_interaction_value(100 + get_random_uniform(9901))
{
}

void WindowEventTimingState::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(entries_to_be_queued);
    for (auto& it : pending_key_downs)
        visitor.visit(it.value);
    for (auto& it : pending_pointer_downs)
        visitor.visit(it.value);
    visitor.visit(event_counts);
}

void for_each_event_type_considered_for_event_timing(Function<void(Utf16FlyString const&)> const& callback)
{
    // MouseEvents
    callback(UIEvents::EventNames::auxclick);
    callback(UIEvents::EventNames::click);
    callback(UIEvents::EventNames::contextmenu);
    callback(UIEvents::EventNames::dblclick);
    callback(UIEvents::EventNames::mousedown);
    callback(UIEvents::EventNames::mouseenter);
    callback(UIEvents::EventNames::mouseleave);
    callback(UIEvents::EventNames::mouseout);
    callback(UIEvents::EventNames::mouseover);
    callback(UIEvents::EventNames::mouseup);

    // PointerEvents
    callback(UIEvents::EventNames::pointerover);
    callback(UIEvents::EventNames::pointerenter);
    callback(UIEvents::EventNames::pointerdown);
    callback(UIEvents::EventNames::pointerup);
    callback(UIEvents::EventNames::pointercancel);
    callback(UIEvents::EventNames::pointerout);
    callback(UIEvents::EventNames::pointerleave);
    callback(UIEvents::EventNames::gotpointercapture);
    callback(UIEvents::EventNames::lostpointercapture);

    // TouchEvents
    static NeverDestroyed<Utf16FlyString> touchstart { "touchstart"_utf16_fly_string };
    static NeverDestroyed<Utf16FlyString> touchend { "touchend"_utf16_fly_string };
    static NeverDestroyed<Utf16FlyString> touchcancel { "touchcancel"_utf16_fly_string };
    callback(*touchstart);
    callback(*touchend);
    callback(*touchcancel);

    // KeyboardEvents
    callback(UIEvents::EventNames::keydown);
    callback(UIEvents::EventNames::keypress);
    callback(UIEvents::EventNames::keyup);

    // InputEvents
    callback(UIEvents::EventNames::beforeinput);
    callback(UIEvents::EventNames::input);

    // CompositionEvents
    callback(UIEvents::EventNames::compositionstart);
    callback(UIEvents::EventNames::compositionupdate);
    callback(UIEvents::EventNames::compositionend);

    // Drag and Drop
    callback(HTML::EventNames::dragstart);
    callback(HTML::EventNames::dragend);
    callback(HTML::EventNames::dragenter);
    callback(HTML::EventNames::dragleave);
    callback(HTML::EventNames::dragover);
    callback(HTML::EventNames::drop);
}

HTML::Window* relevant_window_of_event_target(DOM::EventTarget& target)
{
    if (auto* window = as_if<HTML::Window>(target))
        return window;
    if (auto* node = as_if<DOM::Node>(target))
        return HTML::window_from_global_object(HTML::relevant_global_object(*node));
    return nullptr;
}

// https://w3c.github.io/event-timing/#considered-for-event-timing
bool is_considered_for_event_timing(DOM::Event const& event)
{
    // 1. If event's isTrusted attribute value is set to false, return false.
    if (!event.is_trusted())
        return false;

    // 2. If event's type is one of the following: auxclick, click, contextmenu, dblclick, mousedown, mouseenter,
    //    mouseleave, mouseout, mouseover, mouseup, pointerover, pointerenter, pointerdown, pointerup, pointercancel,
    //    pointerout, pointerleave, gotpointercapture, lostpointercapture, touchstart, touchend, touchcancel, keydown,
    //    keypress, keyup, beforeinput, input, compositionstart, compositionupdate, compositionend, dragstart, dragend,
    //    dragenter, dragleave, dragover, drop, return true.
    static NeverDestroyed<HashTable<Utf16FlyString>> types = [] {
        HashTable<Utf16FlyString> types;
        for_each_event_type_considered_for_event_timing([&](Utf16FlyString const& type) {
            types.set(type);
        });
        return types;
    }();
    if (types->contains(event.type()))
        return true;

    // 3. Return false.
    return false;
}

// https://w3c.github.io/event-timing/#increase-interaction-count
static void increase_interaction_count(WindowEventTimingState& state)
{
    // 1. Increase window's user interaction value value by a small number chosen by the user agent.
    state.user_interaction_value += 7;

    // 2. Let interactionCount be window's interactionCount.
    // 3. Set interactionCount to interactionCount + 1.
    ++state.interaction_count;
}

// https://w3c.github.io/event-timing/#compute-interactionid
WebIDL::UnsignedLongLong compute_interaction_id(DOM::Event const& event, HTML::Window& window)
{
    // 1. If event's isTrusted attribute value is false, return 0.
    if (!event.is_trusted())
        return 0;

    // 2. Let type be event's type attribute value.
    auto const& type = event.type();

    // 3. If type is not one among keyup, compositionstart, input, pointercancel, pointerup, click, or contextmenu,
    //    return 0.
    if (type != UIEvents::EventNames::keyup && type != UIEvents::EventNames::compositionstart && type != UIEvents::EventNames::input
        && type != UIEvents::EventNames::pointercancel && type != UIEvents::EventNames::pointerup && type != UIEvents::EventNames::click
        && type != UIEvents::EventNames::contextmenu)
        return 0;

    // 4. Let window be event's relevant global object.
    // NB: The window is passed in by the caller.
    auto& state = window.event_timing_state();

    // 5. Let pendingKeyDowns be window's pending key downs.
    auto& pending_key_downs = state.pending_key_downs;

    // 6. Let pointerMap be window's pointer interaction value map.
    auto& pointer_map = state.pointer_interaction_value_map;

    // 7. Let pendingPointerDowns be window's pending pointer downs.
    auto& pending_pointer_downs = state.pending_pointer_downs;

    // 8. If type is keyup:
    if (type == UIEvents::EventNames::keyup) {
        auto const* keyboard_event = as_if<UIEvents::KeyboardEvent>(event);
        if (!keyboard_event)
            return 0;

        // 1. If event's isComposing attribute value is true, return 0.
        if (keyboard_event->is_composing())
            return 0;

        // 2. Let code be event's keyCode attribute value.
        auto code = keyboard_event->key_code();

        // 3. If pendingKeyDowns[code] does not exist, return 0.
        auto pending_key_down = pending_key_downs.get(code);
        if (!pending_key_down.has_value())
            return 0;

        // 4. Let entry be pendingKeyDowns[code].
        auto entry = pending_key_down.release_value();

        // 5. Increase interaction count on window.
        increase_interaction_count(state);

        // 6. Let interactionId be window's user interaction value value.
        auto interaction_id = state.user_interaction_value;

        // 7. Set entry's interactionId to interactionId.
        entry->set_interaction_id(interaction_id);

        // 8. Add entry to window's entries to be queued.
        state.entries_to_be_queued.append(entry);

        // 9. Remove pendingKeyDowns[code].
        pending_key_downs.remove(code);

        // 10. Return interactionId.
        return interaction_id;
    }

    // 9. If type is compositionstart:
    if (type == UIEvents::EventNames::compositionstart) {
        // 1. For each entry in the values of pendingKeyDowns:
        for (auto& it : pending_key_downs) {
            // 1. Append entry to window's entries to be queued.
            state.entries_to_be_queued.append(it.value);
        }

        // 2. Clear pendingKeyDowns.
        pending_key_downs.clear();

        // 3. Return 0.
        return 0;
    }

    // 10. If type is input:
    if (type == UIEvents::EventNames::input) {
        // 1. If event is not an instance of InputEvent, return 0.
        // NOTE: This check is done to exclude Events for which the type is input but that are not about modified text
        //       content.
        auto const* input_event = as_if<UIEvents::InputEvent>(event);
        if (!input_event)
            return 0;

        // 2. If event's isComposing attribute value is false, return 0.
        if (!input_event->is_composing())
            return 0;

        // 3. Increase interaction count on window.
        increase_interaction_count(state);

        // 4. Return window's user interaction value.
        return state.user_interaction_value;
    }

    // 11. Otherwise (type is pointercancel, pointerup, click, or contextmenu):
    // 1. Let pointerId be event's pointerId attribute value.
    // AD-HOC: We dispatch click and contextmenu events as MouseEvents rather than PointerEvents, so they have no
    //         pointerId. They come from the mouse, so they use the pointerId of the mouse instead.
    WebIDL::Long pointer_id = 0;
    if (auto const* pointer_event = as_if<UIEvents::PointerEvent>(event))
        pointer_id = pointer_event->pointer_id();
    else if (is<UIEvents::MouseEvent>(event))
        pointer_id = UIEvents::mouse_pointer_id;
    else
        return 0;

    // 2. If type is click:
    if (type == UIEvents::EventNames::click) {
        // 1. If pointerMap[pointerId] does not exist, return 0.
        auto value = pointer_map.get(pointer_id);
        if (!value.has_value())
            return 0;

        // 2. Let value be pointerMap[pointerId].
        // 3. Remove pointerMap[pointerId].
        pointer_map.remove(pointer_id);

        // 4. Return value.
        return *value;
    }

    // 3. Assert that type is pointerup, pointercancel, or contextmenu.
    VERIFY(type == UIEvents::EventNames::pointerup || type == UIEvents::EventNames::pointercancel || type == UIEvents::EventNames::contextmenu);

    // 4. If pendingPointerDowns[pointerId] does not exist:
    auto pending_pointer_down = pending_pointer_downs.get(pointer_id);
    if (!pending_pointer_down.has_value()) {
        // 1. If type is contextmenu, return window's user interaction value.
        if (type == UIEvents::EventNames::contextmenu)
            return state.user_interaction_value;

        // 2. If type is pointerup and window's is contextmenu triggered flag is true:
        if (type == UIEvents::EventNames::pointerup && state.is_contextmenu_triggered) {
            // 1. Set window's is contextmenu triggered flag to false.
            state.is_contextmenu_triggered = false;

            // 2. Return window's user interaction value.
            return state.user_interaction_value;
        }

        // 3. Otherwise, return 0.
        return 0;
    }

    // 5. Let pointerDownEntry be pendingPointerDowns[pointerId].
    // 6. Assert that pointerDownEntry is a PerformanceEventTiming entry.
    auto pointer_down_entry = pending_pointer_down.release_value();

    // 7. If type is pointerup or contextmenu:
    if (type == UIEvents::EventNames::pointerup || type == UIEvents::EventNames::contextmenu) {
        // 1. Increase interaction count on window.
        increase_interaction_count(state);

        // 2. Set pointerMap[pointerId] to window's user interaction value.
        pointer_map.set(pointer_id, state.user_interaction_value);

        // 3. Set pointerDownEntry's interactionId to pointerMap[pointerId].
        pointer_down_entry->set_interaction_id(state.user_interaction_value);
    }

    // 8. Append pointerDownEntry to window’s entries to be queued.
    state.entries_to_be_queued.append(pointer_down_entry);

    // 9. Remove pendingPointerDowns[pointerId].
    pending_pointer_downs.remove(pointer_id);

    // 10. If type is contextmenu, set window's is contextmenu triggered to true.
    if (type == UIEvents::EventNames::contextmenu)
        state.is_contextmenu_triggered = true;

    // 11. If type is pointercancel, return 0.
    if (type == UIEvents::EventNames::pointercancel)
        return 0;

    // 12. Return pointerMap[pointerId].
    return pointer_map.get(pointer_id).value_or(0);
}

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ptr<PerformanceEventTiming> initialize_event_timing(DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id)
{
    // 1. If the algorithm to determine if event should be considered for Event Timing returns false, then return null.
    if (!is_considered_for_event_timing(event))
        return nullptr;

    // 2. Let timingEntry be a new PerformanceEventTiming object with event's relevant realm.
    // 3. Set timingEntry's name to event's type attribute value.
    // 4. Set timingEntry's entryType to "event".
    // 5. Set timingEntry's startTime to event's timeStamp attribute value.
    // 6. Set timingEntry's processingStart to processingStart.
    // 7. Set timingEntry's cancelable to event's cancelable attribute value.
    // 8. Set timingEntry's interactionId to interactionId.
    // 9. Return timingEntry.
    return PerformanceEventTiming::create(event, processing_start, interaction_id);
}

// https://w3c.github.io/event-timing/#generate-a-css-selector
static Utf16String generate_a_css_selector(DOM::EventTarget const& target)
{
    // 1. If target is a Node, run the following steps:
    if (auto const* node = as_if<DOM::Node>(target)) {
        // 1. Let selector be a string with an initial value of target's nodeName.
        Utf16StringBuilder selector;
        selector.append(node->node_name().view());

        // 2. If target is an Element, run the following steps:
        if (auto const* element = as_if<DOM::Element>(*node)) {
            // 1. If target has an id attribute, set selector to the concatenation of « selector, "#", the value of the
            //    id attribute ».
            if (auto id = element->get_attribute(HTML::AttributeNames::id); id.has_value()) {
                selector.append_ascii('#');
                selector.append(*id);
            }
            // 2. Otherwise, if target has a src attribute, set selector to the concatenation of « selector, "[src=",
            //    the value of the src attribute, "]" ».
            else if (auto src = element->get_attribute(HTML::AttributeNames::src); src.has_value()) {
                selector.append_ascii("[src="sv);
                selector.append(*src);
                selector.append_ascii(']');
            }
        }

        // 3. Return selector.
        return selector.to_string();
    }

    // 2. Otherwise, return an empty string.
    return {};
}

// https://w3c.github.io/event-timing/#finalize-event-timing
void finalize_event_timing(GC::Ptr<PerformanceEventTiming> timing_entry, DOM::Event const& event, DOM::EventTarget& target, HighResolutionTime::DOMHighResTimeStamp processing_end)
{
    // 1. If timingEntry is null, then return.
    if (!timing_entry)
        return;

    // 2. Let relevantGlobal be target's relevant global object.
    // 3. If relevantGlobal does not implement Window, return.
    auto* relevant_global = relevant_window_of_event_target(target);
    if (!relevant_global)
        return;
    auto& state = relevant_global->event_timing_state();

    // 4. Set timingEntry's processingEnd to processingEnd.
    timing_entry->set_processing_end(processing_end);

    // 5. Assert that target implements Node.
    // NOTE: This assertion holds due to the types of events supported by the Event Timing API.
    // NB: Events of these types can also be dispatched at the Window or the Document, which have no element to time.
    auto* target_node = as_if<DOM::Node>(target);

    // 6. Set timingEntry's eventTarget to target.
    // NOTE: This will set eventTarget to the last event target. So if retargeting occurs, the last target, closest to
    //       the root, will be used.
    timing_entry->set_event_target(target_node);

    // 7. Set timingEntry's targetSelector to the result of running the algorithm to generate a CSS selector with
    //    target as input.
    timing_entry->set_target_selector(generate_a_css_selector(target));

    // 8. If event's type attribute value is pointerdown:
    if (event.type() == UIEvents::EventNames::pointerdown) {
        // 1. Let pendingPointerDowns be relevantGlobal's pending pointer downs.
        auto& pending_pointer_downs = state.pending_pointer_downs;

        // 2. Let pointerId be event's pointerId.
        auto const* pointer_event = as_if<UIEvents::PointerEvent>(event);
        auto pointer_id = pointer_event ? pointer_event->pointer_id() : 0;

        // 3. If pendingPointerDowns[pointerId] exists:
        if (auto previous_pointer_down_entry = pending_pointer_downs.get(pointer_id); previous_pointer_down_entry.has_value()) {
            // 1. Let previousPointerDownEntry be pendingPointerDowns[pointerId].
            // 2. Add previousPointerDownEntry to relevantGlobal's entries to be queued.
            state.entries_to_be_queued.append(*previous_pointer_down_entry);
        }

        // 4. Set pendingPointerDowns[pointerId] to timingEntry.
        pending_pointer_downs.set(pointer_id, *timing_entry);

        // 5. Set relevantGlobal's is contextmenu triggered to false.
        state.is_contextmenu_triggered = false;
    }
    // 9. Otherwise, if event's type attribute value is keydown:
    else if (auto const* keyboard_event = as_if<UIEvents::KeyboardEvent>(event); keyboard_event && event.type() == UIEvents::EventNames::keydown) {
        // 1. If event's isComposing attribute value is true:
        if (keyboard_event->is_composing()) {
            // 1. Append timingEntry to relevantGlobal's entries to be queued.
            state.entries_to_be_queued.append(*timing_entry);

            // 2. Return.
            return;
        }

        // 2. Let pendingKeyDowns be relevantGlobal's pending key downs.
        auto& pending_key_downs = state.pending_key_downs;

        // 3. Let code be event's keyCode attribute value.
        auto code = keyboard_event->key_code();

        // 4. If pendingKeyDowns[code] exists:
        if (auto previous_key_down_entry = pending_key_downs.get(code); previous_key_down_entry.has_value()) {
            // 1. Let previousKeyDownEntry be pendingKeyDowns[code].
            // 2. If code is not 229:
            if (code != 229) {
                // 1. Increase relevantGlobal's user interaction value value by a small number chosen by the user
                //    agent.
                state.user_interaction_value += 7;

                // 2. Set previousKeyDownEntry's interactionId to relevantGlobal's user interaction value.
                (*previous_key_down_entry)->set_interaction_id(state.user_interaction_value);
            }
            // NOTE: 229 is a special case since it corresponds to IME keyboard events. Sometimes multiple of these
            //       are sent by the user agent, and they do not correspond to holding a key down repeatedly.

            // 3. Add previousKeyDownEntry to relevantGlobal's entries to be queued.
            state.entries_to_be_queued.append(*previous_key_down_entry);
        }

        // 5. Set pendingKeyDowns[code] to timingEntry.
        pending_key_downs.set(code, *timing_entry);
    }
    // 10. Otherwise:
    else {
        // 1. Append timingEntry to relevantGlobal's entries to be queued.
        state.entries_to_be_queued.append(*timing_entry);
    }

    // AD-HOC: The duration of the entry is set by the next rendering update, which is asked for here in case nothing
    //         else asks for one.
    relevant_global->associated_document().page().client().request_frame();
}

// https://w3c.github.io/event-timing/#set-event-timing-entry-duration
static void set_event_timing_entry_duration(PerformanceEventTiming& timing_entry, HTML::Window& window, HighResolutionTime::DOMHighResTimeStamp rendering_timestamp)
{
    auto& state = window.event_timing_state();

    // 1. If timingEntry's duration attribute value is nonzero, return.
    // AD-HOC: A duration that rounds to zero is a duration that has been set all the same. Otherwise, a pending
    //         pointerdown or keydown entry would be given a duration again by every later rendering update until its
    //         duration rounds to a nonzero value, and its event would be counted in eventCounts each time.
    if (timing_entry.has_duration())
        return;

    // 2. Let start be timingEntry's startTime attribute value.
    auto start = timing_entry.start_time();

    // 3. Set timingEntry's duration to a DOMHighResTimeStamp resulting from renderingTimestamp - start, with
    //    granularity of 8ms or less.
    timing_entry.set_duration(round((rendering_timestamp - start) / 8) * 8);

    // 4. Let name be timingEntry's name attribute value.
    Utf16FlyString name { timing_entry.name() };

    // 5. Perform the following steps to update the event counts:
    // 1. Let eventCounts be window's eventCounts.
    // 2. Assert that eventCounts contains name.
    // 3. Set eventCounts[name] to eventCounts[name] + 1.
    window.performance()->event_counts()->increment(name);

    // 6. If window's has dispatched input event is false, and timingEntry's interactionId is not 0, run the following
    //    steps:
    if (!state.has_dispatched_input_event && timing_entry.interaction_id() != 0) {
        // 1. Let firstInputEntry be a copy of timingEntry.
        // 2. Set firstInputEntry's entryType to "first-input".
        auto first_input_entry = timing_entry.copy_with_entry_type(PerformanceTimeline::EntryTypes::first_input);

        // 3. Queue firstInputEntry.
        window.queue_performance_entry(first_input_entry);
        window.add_performance_entry(first_input_entry, HTML::WindowOrWorkerGlobalScopeMixin::CheckIfPerformanceBufferIsFull::Yes);

        // 4. Set window's has dispatched input event to true.
        state.has_dispatched_input_event = true;
    }
}

// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
void dispatch_pending_event_timing_entries(DOM::Document& document)
{
    // 1. Let window be doc's relevant global object.
    auto& window = HTML::relevant_window(document);
    auto& state = window.event_timing_state();

    // OPTIMIZATION: There is nothing to do while no event has been timed.
    if (state.entries_to_be_queued.is_empty() && state.pending_pointer_downs.is_empty() && state.pending_key_downs.is_empty())
        return;

    // 2. Let renderingTimestamp be the current high resolution time.
    auto rendering_timestamp = HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));

    // 3. For each timingEntry in window's entries to be queued:
    for (auto& timing_entry : state.entries_to_be_queued) {
        // 1. Set event timing entry duration passing timingEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(timing_entry, window, rendering_timestamp);

        // 2. If timingEntry's duration attribute value is greater than or equal to 16, then queue timingEntry.
        if (timing_entry->duration() >= 16) {
            window.queue_performance_entry(timing_entry);
            window.add_performance_entry(timing_entry, HTML::WindowOrWorkerGlobalScopeMixin::CheckIfPerformanceBufferIsFull::Yes);
        }
    }

    // 4. Set window's entries to be queued to an empty list.
    state.entries_to_be_queued.clear();

    // 5. For each pendingPointerDownEntry in the values from window's pending pointer downs:
    for (auto& it : state.pending_pointer_downs) {
        // 1. Set event timing entry duration passing pendingPointerDownEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(it.value, window, rendering_timestamp);
    }

    // 6. For each pendingKeyDownEntry in the values from window's pending key downs:
    for (auto& it : state.pending_key_downs) {
        // 1. Set event timing entry duration passing pendingKeyDownEntry, window, and renderingTimestamp.
        set_event_timing_entry_duration(it.value, window, rendering_timestamp);
    }
}

}
