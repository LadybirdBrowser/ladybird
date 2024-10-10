/*
 * Copyright (c) 2024, Noah Bright <noah.bright.1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PerformanceEventTimingPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/EventTiming/PerformanceEventTiming.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/PerformanceTimeline/EntryTypes.h>
#include <LibWeb/PerformanceTimeline/PerformanceObserver.h>
#include <LibWeb/UIEvents/InputEvent.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>
#include <LibWeb/UIEvents/PointerEvent.h>

namespace Web::EventTiming {

static unsigned long long compute_interaction_id(DOM::Event const&);

JS_DEFINE_ALLOCATOR(PerformanceEventTiming);

// https://www.w3.org/TR/event-timing/#sec-init-event-timing
PerformanceEventTiming::PerformanceEventTiming(JS::Realm& realm, String const& name, HighResolutionTime::DOMHighResTimeStamp start_time, HighResolutionTime::DOMHighResTimeStamp duration,
    DOM::Event const& event, HighResolutionTime::DOMHighResTimeStamp processing_start)
    : PerformanceTimeline::PerformanceEntry(realm, name, start_time, duration)
    , m_entry_type(PerformanceTimeline::EntryTypes::event)
    , m_start_time(event.time_stamp())
    , m_processing_start(processing_start)
    , m_cancelable(event.cancelable())
//, m_interaction_id(compute_interaction_id(event))
{
    m_interaction_id = compute_interaction_id(event);
}

// https://www.w3.org/TR/event-timing/#sec-increasing-interaction-count
static void increase_interaction_count(HTML::Window& window)
{
    // 1. Increase window’s user interaction value value by a small number chosen by the user agent.
    //
    // picking lucky number 7 arbitrarily
    // interaction value is initialized to a random between 100 and 10000, so I guess 7 is "small"?
    window.increase_user_interaction_value(7);

    // 2. Let interactionCount be window’s interactionCount.
    auto interaction_count = window.user_interaction_value();

    // 3. Set interactionCount to interactionCount + 1.
    window.set_user_interaction_value(interaction_count + 1);
}

PerformanceEventTiming::~PerformanceEventTiming() = default;

FlyString const& PerformanceEventTiming::entry_type() const
{
    return m_entry_type;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEventTiming::processing_end() const
{
    // The processingEnd attribute’s getter returns a timestamp captured at the end of the event dispatch
    // algorithm. This is when event handlers have finished executing. It’s equal to processingStart when
    // there are no such event handlers.
    dbgln("FIXME: Implement PeformanceEventTiming processing_end()");
    return 0;
}

HighResolutionTime::DOMHighResTimeStamp PerformanceEventTiming::processing_start() const
{
    // The processingStart attribute’s getter returns a timestamp captured at the beginning of
    // the event dispatch algorithm. This is when event handlers are about to be executed.
    //
    // https://dom.spec.whatwg.org/#concept-event-dispatch
    // This is implemented in EventDispatcher::dispatch but not obvious
    // where it captures a timestamp before launching the handlers
    dbgln("FIXME: Implement PeformanceEventTiming processing_start()");
    return 0;
}

bool PerformanceEventTiming::cancelable() const
{
    return m_cancelable;
}

JS::ThrowCompletionOr<JS::GCPtr<DOM::Node>> PerformanceEventTiming::target()
{
    // The target attribute’s getter returns the associated event’s last target when such
    // Node is not disconnected nor in the shadow DOM.
    dbgln("FIXME: Implement PerformanceEventTiming::PeformanceEventTiming target()");
    return nullptr;
}

unsigned long long PerformanceEventTiming::interaction_id() const
{
    return m_interaction_id;
}

// https://www.w3.org/TR/event-timing/#sec-should-add-performanceeventtiming
PerformanceTimeline::ShouldAddEntry PerformanceEventTiming::should_add_performance_event_timing(Optional<PerformanceTimeline::PerformanceObserverInit const&> options) const
{
    dbgln("FIXME: Implement PeformanceEventTiming should_add_performance_event_timing()");
    // 1. If entry’s entryType attribute value equals to "first-input", return true.
    if (entry_type() == "first-input")
        return PerformanceTimeline::ShouldAddEntry::Yes;

    // 2. Assert that entry’s entryType attribute value equals "event".
    VERIFY(entry_type() == "event");

    // 3. Let minDuration be computed as follows:
    HighResolutionTime::DOMHighResTimeStamp min_duration;

    // 3.1. If options is not present or if options’s durationThreshold is not present, let minDuration be 104.
    if (!options.has_value() || options.value().duration_threshold.has_value())
        min_duration = 104.0;
    // 3.2. Otherwise, let minDuration be the maximum between 16 and options’s durationThreshold value.
    else
        min_duration = max(16.0, options.value().duration_threshold.value());

    // 4. If entry’s duration attribute value is greater than or equal to minDuration, return true.
    if (duration() >= min_duration)
        return PerformanceTimeline::ShouldAddEntry::Yes;

    // 5. Otherwise, return false.
    return PerformanceTimeline::ShouldAddEntry::No;
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-availablefromtimeline
// FIXME: the output here depends on the type of the object instance, but this function is static
//        the commented out if statement won't compile
PerformanceTimeline::AvailableFromTimeline PerformanceEventTiming::available_from_timeline()
{
    dbgln("FIXME: Implement PeformanceEventTiming available_from_timeline()");
    // if (entry_type() == "first-input")
    return PerformanceTimeline::AvailableFromTimeline::Yes;
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-maxbuffersize
// FIXME: Same issue as available_from_timeline() above
Optional<u64> PerformanceEventTiming::max_buffer_size()
{
    dbgln("FIXME: Implement PeformanceEventTiming max_buffer_size()");
    if (true) //(entry_type() == "first-input")
        return 1;
    // else return 150;
}

static unsigned long long compute_interaction_id(DOM::Event const& event)
{
    // some ad hoc verify casts are thrown in for each branch depending on the
    // event type

    // 1. If event’s isTrusted attribute value is false, return 0.
    if (!event.is_trusted())
        return 0;

    // 2. Let type be event’s type attribute value.
    auto const& type = event.type();

    // 3. If type is not one among keyup, compositionstart, input, pointercancel, pointermove, pointerup, or click, return 0.
    // FIXME: Any reason not to wrap these in an enum?
    if (type != "keyup"
        || type != "compositionstart"
        || type != "input"
        || type != "pointercancel"
        || type != "pointermove"
        || type != "pointerup"
        || type != "click")
        return 0;

    // Note: keydown and pointerdown are handled in finalize event timing.

    // 4. Let window be event’s relevant global object.
    auto& object = event.realm().global_object();

    // FIXME: Get the global object, but then run a bunch of algorithms this spec
    //        gives for windows. Throwing in an ad hoc cast
    if (!is<HTML::Window>(object))
        return 0;
    auto& window = verify_cast<HTML::Window>(object);

    // 5. Let pendingKeyDowns be window’s pending key downs.
    auto& pending_key_downs = window.pending_key_downs();

    // 6. Let pointerMap be window’s pointer interaction value map.
    auto& pointer_map = window.pointer_interaction_value_map();

    // 7. Let pointerIsDragSet be window’s pointer is drag set.
    auto& pointer_is_drag_set = window.pointer_is_drag_set();

    // 8. Let pendingPointerDowns be window’s pending pointer downs.
    auto& pending_pointer_downs = window.pending_pointer_downs();

    // 9. If type is keyup:
    if (type == "keyup") {
        // https://www.w3.org/TR/uievents/#dom-keyboardevent-iscomposing
        auto const& keyup_event = verify_cast<UIEvents::KeyboardEvent>(event);

        // 9.1. If event’s isComposing attribute value is true, return 0.
        if (keyup_event.is_composing())
            return 0;

        // 9.2. Let code be event’s keyCode attribute value.
        auto code = static_cast<int>(keyup_event.key_code());

        // 9.3. If pendingKeyDowns[code] does not exist, return 0.
        if (!pending_key_downs.contains(code))
            return 0;

        // 9.4. Let entry be pendingKeyDowns[code].
        auto entry = pending_key_downs.get(code);

        // 9.5. Increase interaction count on window.
        increase_interaction_count(window);

        // 9.6. Let interactionId be window’s user interaction value value.
        auto interaction_id = window.user_interaction_value();

        // 9.7. Set entry’s interactionId to interactionId.
        entry->set_interaction_id(interaction_id);

        // 9.8. Add entry to window’s entries to be queued.
        window.entries_to_be_queued().append(entry.value());

        // 9.9. Remove pendingKeyDowns[code].
        pending_key_downs.remove(code);

        // 9.10. Return interactionId.
        return interaction_id;
    }

    // 10. If type is compositionstart:
    if (type == "compositionstart") {
        // 10.1. For each entry in the values of pendingKeyDowns:
        for (auto& [_, entry] : pending_key_downs)
            // 10.1.1 Append entry to window’s entries to be queued.
            window.entries_to_be_queued().append(entry);

        // 10.2. Clear pendingKeyDowns.
        pending_key_downs.clear();

        // 10.3. Return 0.
        return 0;
    }

    // 11. If type is input:
    if (type == "input") {

        // 11.1 If event is not an instance of InputEvent, return 0.
        // Note: this check is done to exclude Events for which the type is input but that are not about modified text content.
        if (!is<UIEvents::InputEvent>(event))
            return 0;

        auto const& input_event = verify_cast<UIEvents::InputEvent>(event);

        // 11.2 If event’s isComposing attribute value is false, return 0.
        if (!input_event.is_composing())
            return 0;

        // 11.3 Increase interaction count on window.
        increase_interaction_count(window);

        // 11.4 Return window’s user interaction value.
        return window.user_interaction_value();
    }

    // 12. Otherwise (type is pointercancel, pointermove, pointerup, or click):
    auto const& pointer_event = verify_cast<UIEvents::PointerEvent>(event);

    // 12.1. Let pointerId be event’s pointerId attribute value.
    auto pointer_id = pointer_event.pointer_id();

    // 12.2. If type is click:
    if (pointer_event.type() == "click") {
        // 12.1.1. If pointerMap[pointerId] does not exist, return 0.
        if (pointer_map.find(pointer_id) == pointer_map.end())
            return 0;

        // 12.1.2. Let value be pointerMap[pointerId].
        auto value = pointer_map.get(pointer_id);

        // 12.1.3. Remove pointerMap[pointerId].
        pointer_map.remove(pointer_id);

        // 12.1.4. Remove [pointerId] from pointerIsDragSet.
        pointer_is_drag_set.remove(pointer_id);

        // 12.1.5. Return value.
        return value.value();
    }

    // 12.3. If type is pointermove:
    if (pointer_event.type() == "pointermove") {
        // 12.1. Add pointerId to pointerIsDragSet.
        pointer_is_drag_set.set(pointer_id);

        // 12.1. Return 0.
        return 0;
    }

    // 12.4. Assert that type is pointerup or pointercancel.
    VERIFY(type == "pointerup" || type == "pointercancel");

    // 12.5. If pendingPointerDowns[pointerId] does not exist, return 0.
    if (pending_pointer_downs.find(pointer_id) == pending_pointer_downs.end())
        return 0;

    // 12.6. Let pointerDownEntry be pendingPointerDowns[pointerId].
    // 12.7. Assert that pointerDownEntry is a PerformanceEventTiming entry.
    auto pointer_down_entry = pending_pointer_downs.get(pointer_id);

    // 12.8. If type is pointerup:
    if (type == "pointerup") {

        // 12.8.1. Let interactionType be "tap".
        auto const* interaction_type = "tap";

        // 12.8.2. If pointerIsDragSet contains [pointerId] exists, set interactionType to "drag".
        if (pointer_is_drag_set.find(pointer_id) != pointer_is_drag_set.end())
            interaction_type = "drag";

        // This is unused?
        (void)interaction_type;

        // 12.8.3. Increase interaction count on window.
        increase_interaction_count(window);

        // 12.8.4. Set pointerMap[pointerId] to window’s user interaction value.
        pointer_map.set(pointer_id, window.user_interaction_value());

        // 12.8.5. Set pointerDownEntry’s interactionId to pointerMap[pointerId].
        pointer_down_entry.value().set_interaction_id(pointer_map.get(pointer_id).value());
    }

    // 12.9. Append pointerDownEntry to window’s entries to be queued.
    window.entries_to_be_queued().append(pointer_down_entry.value());

    // 12.10. Remove pendingPointerDowns[pointerId].
    pending_pointer_downs.remove(pointer_id);

    // 12.11. If type is pointercancel, return 0.
    if (type == "pointercancel")
        return 0;

    // 12.12. Return pointerMap[pointerId].
    return pointer_map.get(pointer_id).value();
}

// https://w3c.github.io/timing-entrytypes-registry/#dfn-should-add-entry
PerformanceTimeline::ShouldAddEntry PerformanceEventTiming::should_add_entry(Optional<PerformanceTimeline::PerformanceObserverInit const&>) const
{
    return should_add_performance_event_timing();
}

void PerformanceEventTiming::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PerformanceEventTiming);
}

void PerformanceEventTiming::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_target);
}

}
