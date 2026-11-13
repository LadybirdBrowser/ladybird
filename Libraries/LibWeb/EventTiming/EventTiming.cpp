/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/NeverDestroyed.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/EventTiming/EventTiming.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/UIEvents/EventNames.h>

namespace Web::EventTiming {

extern "C" {
// Defined in Bindings/Rust/src/event_timing/event_timing.rs.
u64 web_rust_event_timing_compute_interaction_id(JSVM*, HTML::Window*, DOM::Event const*);
PerformanceEventTiming* web_rust_event_timing_initialize_event_timing(JSVM*, DOM::Event const*, double processing_start, u64 interaction_id);
void web_rust_event_timing_finalize_event_timing(JSVM*, PerformanceEventTiming*, DOM::Event const*, HTML::Window*, DOM::Node*, FlatPtr target_selector, double processing_end);
bool web_rust_event_timing_has_pending_event_timing_entries(JSVM*, HTML::Window*);
void web_rust_event_timing_dispatch_pending_event_timing_entries(JSVM*, HTML::Window*);
bool web_rust_event_timing_has_dispatched_input_event(JSVM*, HTML::Window*);

// Called by Bindings/Rust/src/event_timing.
void web_event_timing_for_each_event_type_considered_for_event_timing(void* context, void (*callback)(void* context, FlatPtr event_type));
void web_event_timing_request_frame(HTML::Window*);
}

static void for_each_event_type_considered_for_event_timing(Function<void(Utf16FlyString const&)> const& callback)
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

extern "C" {

void web_event_timing_for_each_event_type_considered_for_event_timing(void* context, void (*callback)(void* context, FlatPtr event_type))
{
    for_each_event_type_considered_for_event_timing([&](Utf16FlyString const& event_type) {
        callback(context, event_type.raw_identity());
    });
}

void web_event_timing_request_frame(HTML::Window* window)
{
    window->associated_document().page().client().request_frame();
}
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

// https://w3c.github.io/event-timing/#compute-interactionid
WebIDL::UnsignedLongLong compute_interaction_id(DOM::Event const& event, HTML::Window& window)
{
    return web_rust_event_timing_compute_interaction_id(JS::HostABI::vm_to_abi(JS::VM::the()), &window, &event);
}

// https://w3c.github.io/event-timing/#initialize-event-timing
GC::Ptr<PerformanceEventTiming> initialize_event_timing(DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start, WebIDL::UnsignedLongLong interaction_id)
{
    // 1. If the algorithm to determine if event should be considered for Event Timing returns false, then return null.
    if (!is_considered_for_event_timing(event))
        return nullptr;

    return web_rust_event_timing_initialize_event_timing(JS::HostABI::vm_to_abi(JS::VM::the()), &event, processing_start, interaction_id);
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

    web_rust_event_timing_finalize_event_timing(
        JS::HostABI::vm_to_abi(JS::VM::the()),
        timing_entry.ptr(),
        &event,
        relevant_global,
        as_if<DOM::Node>(target),
        generate_a_css_selector(target).into_raw(),
        processing_end);
}

bool has_pending_event_timing_entries(DOM::Document const& document)
{
    auto window = document.window();
    if (!window)
        return false;
    return web_rust_event_timing_has_pending_event_timing_entries(JS::HostABI::vm_to_abi(JS::VM::the()), window.ptr());
}

// https://w3c.github.io/event-timing/#dispatch-pending-event-timing-entries
void dispatch_pending_event_timing_entries(DOM::Document& document)
{
    // 1. Let window be doc's relevant global object.
    auto& window = HTML::relevant_window(document);
    web_rust_event_timing_dispatch_pending_event_timing_entries(JS::HostABI::vm_to_abi(JS::VM::the()), &window);
}

bool has_dispatched_input_event(HTML::Window& window)
{
    return web_rust_event_timing_has_dispatched_input_event(JS::HostABI::vm_to_abi(JS::VM::the()), &window);
}

}
