/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Event.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/BindingsGlue.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Event);

static_assert(sizeof(RustFFI::EventPathItem) == 16 && alignof(RustFFI::EventPathItem) == 4);
static_assert(offsetof(RustFFI::EventPathItem, invocation_target) == 0);
static_assert(offsetof(RustFFI::EventPathItem, shadow_adjusted_target) == 4);
static_assert(offsetof(RustFFI::EventPathItem, related_target) == 8);
static_assert(offsetof(RustFFI::EventPathItem, invocation_target_in_shadow_tree) == 12);
static_assert(offsetof(RustFFI::EventPathItem, root_of_closed_tree) == 13);
static_assert(offsetof(RustFFI::EventPathItem, slot_in_closed_tree) == 14);
static_assert(sizeof(RustFFI::EventPath) == 32 && alignof(RustFFI::EventPath) == 8);
static_assert(offsetof(RustFFI::EventPath, items) == 0);
static_assert(offsetof(RustFFI::EventPath, size) == 8);
static_assert(offsetof(RustFFI::EventPath, capacity) == 12);
static_assert(offsetof(RustFFI::EventPath, touch_targets) == 16);
static_assert(offsetof(RustFFI::EventPath, touch_target_count) == 24);
static_assert(sizeof(RustFFI::EventCore) == 56 && alignof(RustFFI::EventCore) == 8);
static_assert(offsetof(RustFFI::EventCore, target) == 0);
static_assert(offsetof(RustFFI::EventCore, related_target) == 4);
static_assert(offsetof(RustFFI::EventCore, current_target) == 8);
static_assert(offsetof(RustFFI::EventCore, event_phase) == 12);
static_assert(offsetof(RustFFI::EventCore, bubbles) == 13);
static_assert(offsetof(RustFFI::EventCore, cancelable) == 14);
static_assert(offsetof(RustFFI::EventCore, stop_propagation) == 15);
static_assert(offsetof(RustFFI::EventCore, stop_immediate_propagation) == 16);
static_assert(offsetof(RustFFI::EventCore, canceled) == 17);
static_assert(offsetof(RustFFI::EventCore, in_passive_listener) == 18);
static_assert(offsetof(RustFFI::EventCore, composed) == 19);
static_assert(offsetof(RustFFI::EventCore, initialized) == 20);
static_assert(offsetof(RustFFI::EventCore, dispatch) == 21);
static_assert(offsetof(RustFFI::EventCore, is_trusted) == 22);
static_assert(offsetof(RustFFI::EventCore, path) == 24);
static_assert(Event::Phase::None == RustFFI::EVENT_PHASE_NONE);
static_assert(Event::Phase::CapturingPhase == RustFFI::EVENT_PHASE_CAPTURING);
static_assert(Event::Phase::AtTarget == RustFFI::EVENT_PHASE_AT_TARGET);
static_assert(Event::Phase::BubblingPhase == RustFFI::EVENT_PHASE_BUBBLING);

// https://dom.spec.whatwg.org/#concept-event-create
GC::Ref<Event> Event::create(JS::Object const& relevant_global_object, FlyString const& event_name, EventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

GC::Ref<Event> Event::create(JS::Object const& relevant_global_object, Utf16FlyString const& event_name, EventInit const& event_init)
{
    return create(FlyString { event_name.to_utf16_string().to_utf8() }, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object));
}

// https://dom.spec.whatwg.org/#concept-event-create
GC::Ref<Event> Event::create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<Event>(event_name, time_stamp);
    // 4. Initialize event’s isTrusted attribute to true.
    event->m_event_core.is_trusted = true;
    return event;
}

GC::Ref<Event> Event::create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(FlyString { event_name.to_utf16_string().to_utf8() }, time_stamp);
}

GC::Ref<Event> Event::create_bubbling_composed(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = create(event_name, time_stamp);
    event->set_bubbles(true);
    event->set_composed(true);
    return event;
}

GC::Ref<Event> Event::create_bubbling_composed(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = create(event_name, time_stamp);
    event->set_bubbles(true);
    event->set_composed(true);
    return event;
}

// https://dom.spec.whatwg.org/#concept-event-create
GC::Ref<Event> Event::create(FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<Event>(event_name, event_init, time_stamp);
    // 4. Initialize event’s isTrusted attribute to true.
    event->m_event_core.is_trusted = true;
    return event;
}

GC::Ref<Event> Event::create(Utf16FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(FlyString { event_name.to_utf16_string().to_utf8() }, event_init, time_stamp);
}

GC::Ref<Event> Event::create_for_constructor(FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<Event>(event_name, event_init, time_stamp);
}

GC::Ref<Event> Event::create_for_constructor(FlyString const& event_name, Bindings::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<Event>(event_name, event_init, time_stamp);
}

GC::Ref<Event> Event::create_for_constructor(Utf16String const& event_name, Bindings::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<Event>(Utf16FlyString::from_utf16(event_name.utf16_view()), event_init, time_stamp);
}

// https://dom.spec.whatwg.org/#inner-event-creation-steps
Event::Event(FlyString const& type, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : m_type(Utf16FlyString::from_utf8(type))
    , m_time_stamp(time_stamp)
{
    // NB: libweb_rust finds the core of an event at this offset into the event's cell.
    static_assert(offsetof(Event, m_event_core) == RustFFI::EVENT_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(Event, m_event_core) >= sizeof(Bindings::GCAllocatedWrappable));
    m_event_core.initialized = true;
    m_event_core.is_trusted = true;
}

Event::Event(Utf16FlyString const& type, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Bindings::GCAllocatedWrappable()
    , m_type(type)
    , m_time_stamp(time_stamp)
{
    m_event_core.initialized = true;
    m_event_core.is_trusted = true;
}

// https://dom.spec.whatwg.org/#inner-event-creation-steps
Event::Event(FlyString const& type, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : m_type(Utf16FlyString::from_utf8(type))
    , m_time_stamp(time_stamp)
{
    m_event_core.bubbles = event_init.bubbles;
    m_event_core.cancelable = event_init.cancelable;
    m_event_core.composed = event_init.composed;
    m_event_core.initialized = true;
    m_event_core.is_trusted = true;
}

void Event::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_core.target);
    visitor.visit(m_event_core.related_target);
    visitor.visit(m_event_core.current_target);
    // NB: The path's buffers are allocated by libweb_rust, which writes only items and touch targets that count.
    auto& path = m_event_core.path;
    for (size_t i = 0; i < path.size; ++i) {
        visitor.visit(path.items[i].invocation_target);
        visitor.visit(path.items[i].shadow_adjusted_target);
        visitor.visit(path.items[i].related_target);
    }
    for (size_t i = 0; i < static_cast<size_t>(path.size) * path.touch_target_count; ++i)
        visitor.visit(path.touch_targets[i]);
    visitor.visit(m_touch_target_list);
}

void Event::set_touch_target_list(ReadonlySpan<GC::Member<EventTarget>> touch_targets)
{
    m_touch_target_list.clear_with_capacity();
    for (auto const& touch_target : touch_targets)
        m_touch_target_list.append(touch_target.ptr());
}

void Event::set_cancelled_flag()
{
    if (m_event_core.cancelable && !m_event_core.in_passive_listener)
        m_event_core.canceled = true;
}

// https://dom.spec.whatwg.org/#concept-event-initialize
void Event::initialize_event(Utf16FlyString const& type, bool bubbles, bool cancelable)
{
    // 1. Set event’s initialized flag.
    m_event_core.initialized = true;

    // 2. Unset event’s stop propagation flag, stop immediate propagation flag, and canceled flag.
    m_event_core.stop_propagation = false;
    m_event_core.stop_immediate_propagation = false;
    m_event_core.canceled = false;

    // 3. Set event’s isTrusted attribute to false.
    m_event_core.is_trusted = false;

    // 4. Set event’s target to null.
    m_event_core.target = nullptr;

    // 5. Set event’s type attribute to type.
    m_type = type;

    // 6. Set event’s bubbles attribute to bubbles.
    m_event_core.bubbles = bubbles;

    // 8. Set event’s cancelable attribute to cancelable.
    m_event_core.cancelable = cancelable;
}

// https://dom.spec.whatwg.org/#dom-event-initevent
void Event::init_event(Utf16FlyString const& type, bool bubbles, bool cancelable)
{
    // 1. If this’s dispatch flag is set, then return.
    if (m_event_core.dispatch)
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);
}

// https://dom.spec.whatwg.org/#dom-event-composedpath
Vector<GC::Root<EventTarget>> Event::composed_path() const
{
    // 1. Let composedPath be an empty list.
    Vector<GC::Root<EventTarget>> composed_path;

    // 2. Let path be this’s path. (NOTE: Not necessary)

    // 3. If path is empty, then return composedPath.
    auto path = this->path();
    if (path.is_empty())
        return composed_path;

    // 4. Let currentTarget be this’s currentTarget attribute value. (NOTE: Not necessary)

    // 5. Assert: currentTarget is an EventTarget object.
    VERIFY(m_event_core.current_target);

    // 6. Append currentTarget to composedPath.
    // NOTE: If path is not empty, then the event is being dispatched and will have a currentTarget.
    composed_path.append(const_cast<EventTarget*>(m_event_core.current_target.ptr()));

    // 7. Let currentTargetIndex be 0.
    size_t current_target_index = 0;

    // 8. Let currentTargetHiddenSubtreeLevel be 0.
    size_t current_target_hidden_subtree_level = 0;

    // 9. Let index be path’s size − 1.
    // 10. While index is greater than or equal to 0:
    for (ssize_t index = path.size() - 1; index >= 0; --index) {
        auto& path_entry = path[index];

        // 1. If path[index]'s root-of-closed-tree is true, then increase currentTargetHiddenSubtreeLevel by 1.
        if (path_entry.root_of_closed_tree)
            ++current_target_hidden_subtree_level;

        // 2. If path[index]'s invocation target is currentTarget, then set currentTargetIndex to index and break.
        if (path_entry.invocation_target.ptr() == m_event_core.current_target.ptr()) {
            current_target_index = index;
            break;
        }

        // 3. If path[index]'s slot-in-closed-tree is true, then decrease currentTargetHiddenSubtreeLevel by 1.
        if (path_entry.slot_in_closed_tree)
            --current_target_hidden_subtree_level;

        // 4. Decrease index by 1.
    }

    // 11. Let currentHiddenLevel and maxHiddenLevel be currentTargetHiddenSubtreeLevel.
    size_t current_hidden_level = current_target_hidden_subtree_level;
    size_t max_hidden_level = current_target_hidden_subtree_level;

    // 12. Set index to currentTargetIndex − 1.
    // 13. While index is greater than or equal to 0:
    for (ssize_t index = current_target_index - 1; index >= 0; --index) {
        auto& path_entry = path[index];

        // 1. If path[index]'s root-of-closed-tree is true, then increase currentHiddenLevel by 1.
        if (path_entry.root_of_closed_tree)
            ++current_hidden_level;

        // 2. If currentHiddenLevel is less than or equal to maxHiddenLevel, then prepend path[index]'s invocation target to composedPath.
        if (current_hidden_level <= max_hidden_level) {
            VERIFY(path_entry.invocation_target);
            composed_path.prepend(const_cast<EventTarget*>(path_entry.invocation_target.ptr()));
        }

        // 3. If path[index]'s slot-in-closed-tree is true, then:
        if (path_entry.slot_in_closed_tree) {
            // 1. Decrease currentHiddenLevel by 1.
            --current_hidden_level;

            // 2. If currentHiddenLevel is less than maxHiddenLevel, then set maxHiddenLevel to currentHiddenLevel.
            if (current_hidden_level < max_hidden_level)
                max_hidden_level = current_hidden_level;
        }

        // 4. Decrease index by 1.
    }

    // 14. Set currentHiddenLevel and maxHiddenLevel to currentTargetHiddenSubtreeLevel.
    current_hidden_level = current_target_hidden_subtree_level;
    max_hidden_level = current_target_hidden_subtree_level;

    // 15. Set index to currentTargetIndex + 1.
    // 16. While index is less than path’s size:
    for (size_t index = current_target_index + 1; index < path.size(); ++index) {
        auto& path_entry = path[index];

        // 1. If path[index]'s slot-in-closed-tree is true, then increase currentHiddenLevel by 1.
        if (path_entry.slot_in_closed_tree)
            ++current_hidden_level;

        // 2. If currentHiddenLevel is less than or equal to maxHiddenLevel, then append path[index]'s invocation target to composedPath.
        if (current_hidden_level <= max_hidden_level) {
            VERIFY(path_entry.invocation_target);
            composed_path.append(const_cast<EventTarget*>(path_entry.invocation_target.ptr()));
        }

        // 3. If path[index]'s root-of-closed-tree is true, then:
        if (path_entry.root_of_closed_tree) {
            // 1. Decrease currentHiddenLevel by 1.
            --current_hidden_level;

            // 2. If currentHiddenLevel is less than maxHiddenLevel, then set maxHiddenLevel to currentHiddenLevel.
            if (current_hidden_level < max_hidden_level)
                max_hidden_level = current_hidden_level;
        }

        // 4. Increase index by 1.
    }

    // 17. Return composedPath.
    return composed_path;
}

}

namespace Web::Bindings {

DOM::Event* event_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<DOM::Event>(&value.as_object());
}

DOM::Event* event_from_callback_argument(JS::VM& vm)
{
    return event_from_value(vm.argument(0));
}

JS::Value event(JS::Realm& realm, GC::Ref<DOM::Event> event)
{
    return wrap(host_defined_wrapper_world(realm), realm, event);
}

GC::Ptr<JS::Object> current_target_wrapper(JS::Realm& realm, DOM::Event const& event)
{
    auto current_target = event.current_target();
    if (auto* window = as_if<HTML::Window>(current_target.ptr())) {
        if (auto browsing_context = window->browsing_context())
            return browsing_context->window_proxy_for(Bindings::host_defined_wrapper_world(realm), realm)->object();
        return window->window()->object();
    }
    return wrap(host_defined_wrapper_world(realm), realm, current_target);
}

JS::Value current_target_value(JS::Realm& realm, DOM::Event const& event)
{
    if (auto current_target = current_target_wrapper(realm, event))
        return current_target;
    return JS::js_null();
}

}
