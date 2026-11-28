/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Utf16FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Bindings {

enum class InterfaceName : u16;
class RustInterfaceCell;
struct EventInit;

}

namespace Web::DOM {

struct EventInit {
    bool bubbles { false };
    bool cancelable { false };
    bool composed { false };
};

class WEB_API Event : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Event, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Event);

public:
    virtual Bindings::RustInterfaceCell* rust_event_owner(Bindings::InterfaceName) const { return nullptr; }

    enum Phase : u16 {
        None = 0,
        CapturingPhase = 1,
        AtTarget = 2,
        BubblingPhase = 3,
    };

    // FIXME: These need explicit marking somehow.
    using TouchTargetList = Vector<GC::Ptr<EventTarget>>;

    [[nodiscard]] static GC::Ref<Event> create(JS::Object const& relevant_global_object, FlyString const& event_name, EventInit const& event_init = {});
    [[nodiscard]] static GC::Ref<Event> create(JS::Object const& relevant_global_object, Utf16FlyString const& event_name, EventInit const& event_init = {});
    [[nodiscard]] static GC::Ref<Event> create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create_bubbling_composed(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create_bubbling_composed(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create(FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create(Utf16FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create_for_constructor(FlyString const& event_name, EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create_for_constructor(FlyString const& event_name, Bindings::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<Event> create_for_constructor(Utf16String const& event_name, Bindings::EventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    Event(FlyString const& type, HighResolutionTime::DOMHighResTimeStamp);
    Event(Utf16FlyString const& type, HighResolutionTime::DOMHighResTimeStamp);
    Event(FlyString const& type, EventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    template<typename EventInitLike>
    Event(FlyString const& type, EventInitLike const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
        : Bindings::GCAllocatedWrappable()
        , m_type(Utf16FlyString::from_utf8(type))
        , m_time_stamp(time_stamp)
    {
        m_event_core.bubbles = event_init.bubbles;
        m_event_core.cancelable = event_init.cancelable;
        m_event_core.composed = event_init.composed;
        m_event_core.initialized = true;
        m_event_core.is_trusted = true;
    }

    template<typename EventInitLike>
    Event(Utf16FlyString const& type, EventInitLike const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
        : Bindings::GCAllocatedWrappable()
        , m_type(type)
        , m_time_stamp(time_stamp)
    {
        m_event_core.bubbles = event_init.bubbles;
        m_event_core.cancelable = event_init.cancelable;
        m_event_core.composed = event_init.composed;
        m_event_core.initialized = true;
        m_event_core.is_trusted = true;
    }

    virtual ~Event() = default;

    // https://dom.spec.whatwg.org/#dom-event-timestamp
    HighResolutionTime::DOMHighResTimeStamp time_stamp() const { return m_time_stamp; }

    Utf16FlyString const& type() const { return m_type; }
    void set_type(Utf16FlyString const& type) { m_type = type; }

    GC::Ptr<EventTarget> target() const { return m_event_core.target; }
    void set_target(EventTarget* target) { m_event_core.target = target; }
    static constexpr GC::MemberOffset target_offset() { return GC_FIELD_OFFSET(Event, m_event_core.target); }

    // NOTE: This is intended for the JS bindings.
    GC::Ptr<EventTarget> src_element() const { return target(); }

    GC::Ptr<EventTarget> related_target() const { return m_event_core.related_target; }
    void set_related_target(GC::Ptr<EventTarget> related_target) { m_event_core.related_target = related_target; }
    static constexpr GC::MemberOffset related_target_offset() { return GC_FIELD_OFFSET(Event, m_event_core.related_target); }

    bool cancelled() const { return m_event_core.canceled; }
    void set_cancelled(bool cancelled) { m_event_core.canceled = cancelled; }

    void set_in_passive_listener(bool in_passive_listener) { m_event_core.in_passive_listener = in_passive_listener; }

    bool composed() const { return m_event_core.composed; }
    void set_composed(bool composed) { m_event_core.composed = composed; }

    bool initialized() const { return m_event_core.initialized; }
    void set_initialized(bool initialized) { m_event_core.initialized = initialized; }

    bool dispatched() const { return m_event_core.dispatch; }

    void prevent_default() { set_cancelled_flag(); }
    bool default_prevented() const { return cancelled(); }

    u16 event_phase() const { return m_event_core.event_phase; }

    GC::Ptr<EventTarget> current_target() const { return m_event_core.current_target; }

    bool return_value() const { return !m_event_core.canceled; }
    void set_return_value(bool return_value)
    {
        if (!return_value)
            set_cancelled_flag();
    }

    // https://dom.spec.whatwg.org/#event-path, which the dispatch in libweb_rust builds in the event's core.
    ReadonlySpan<RustFFI::EventPathItem> path() const { return { m_event_core.path.items, m_event_core.path.size }; }

    void set_touch_target_list(ReadonlySpan<GC::Member<EventTarget>>);
    TouchTargetList& touch_target_list() { return m_touch_target_list; }

    bool bubbles() const { return m_event_core.bubbles; }
    void set_bubbles(bool bubbles) { m_event_core.bubbles = bubbles; }

    bool cancelable() const { return m_event_core.cancelable; }
    void set_cancelable(bool cancelable) { m_event_core.cancelable = cancelable; }

    bool is_trusted() const { return m_event_core.is_trusted; }
    void set_is_trusted(bool is_trusted) { m_event_core.is_trusted = is_trusted; }

    void stop_propagation() { m_event_core.stop_propagation = true; }

    bool cancel_bubble() const { return m_event_core.stop_propagation; }
    void set_cancel_bubble(bool cancel_bubble)
    {
        if (cancel_bubble)
            m_event_core.stop_propagation = true;
    }

    void stop_immediate_propagation()
    {
        m_event_core.stop_propagation = true;
        m_event_core.stop_immediate_propagation = true;
    }

    void init_event(Utf16FlyString const&, bool, bool);

    void set_time_stamp(double time_stamp) { m_time_stamp = time_stamp; }

    Vector<GC::Root<EventTarget>> composed_path() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_mouse_event() const { return false; }
    virtual bool is_pointer_event() const { return false; }

protected:
    void initialize_event(Utf16FlyString const&, bool, bool);

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    Utf16FlyString m_type;
    // NB: The constructors initialize isTrusted to true. Every way that script obtains an event initializes it to false:
    //     the bindings for constructors, createEvent(), initEvent(), dispatchEvent() and click().
    RustFFI::EventCore m_event_core {};

    TouchTargetList m_touch_target_list;

    HighResolutionTime::DOMHighResTimeStamp m_time_stamp { 0 };

    void set_cancelled_flag();
};

}
