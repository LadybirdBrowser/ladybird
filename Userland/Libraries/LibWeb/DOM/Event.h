/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::DOM {

struct EventInit {
    bool bubbles { false };
    bool cancelable { false };
    bool composed { false };
};

class Event : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Event, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Event);

public:
    enum Phase : u16 {
        None = 0,
        CapturingPhase = 1,
        AtTarget = 2,
        BubblingPhase = 3,
    };

    // FIXME: These need explicit marking somehow.
    using TouchTargetList = Vector<JS::GCPtr<EventTarget>>;

    struct PathEntry {
        JS::GCPtr<EventTarget> invocation_target;
        bool invocation_target_in_shadow_tree { false };
        JS::GCPtr<EventTarget> shadow_adjusted_target;
        JS::GCPtr<EventTarget> related_target;
        TouchTargetList touch_target_list;
        bool root_of_closed_tree { false };
        bool slot_in_closed_tree { false };
        size_t index;
    };

    using Path = Vector<PathEntry>;

    [[nodiscard]] static JS::NonnullGCPtr<Event> create(JS::Realm&, FlyString const& event_name, EventInit const& event_init = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Event>> construct_impl(JS::Realm&, FlyString const& event_name, EventInit const& event_init);

    Event(JS::Realm&, FlyString const& type);
    Event(JS::Realm&, FlyString const& type, EventInit const& event_init);

    virtual ~Event() = default;

    double time_stamp() const;

    FlyString const& type() const { return m_type; }
    void set_type(FlyString const& type) { m_type = type; }

    JS::GCPtr<EventTarget> target() const { return m_target; }
    void set_target(EventTarget* target) { m_target = target; }

    // NOTE: This is intended for the JS bindings.
    JS::GCPtr<EventTarget> src_element() const { return target(); }

    JS::GCPtr<EventTarget> related_target() const { return m_related_target; }
    void set_related_target(EventTarget* related_target) { m_related_target = related_target; }

    bool should_stop_propagation() const { return m_stop_propagation; }
    void set_stop_propagation(bool stop_propagation) { m_stop_propagation = stop_propagation; }

    bool should_stop_immediate_propagation() const { return m_stop_immediate_propagation; }
    void set_stop_immediate_propagation(bool stop_immediate_propagation) { m_stop_immediate_propagation = stop_immediate_propagation; }

    bool cancelled() const { return m_cancelled; }
    void set_cancelled(bool cancelled) { m_cancelled = cancelled; }

    bool in_passive_listener() const { return m_in_passive_listener; }
    void set_in_passive_listener(bool in_passive_listener) { m_in_passive_listener = in_passive_listener; }

    bool composed() const { return m_composed; }
    void set_composed(bool composed) { m_composed = composed; }

    bool initialized() const { return m_initialized; }
    void set_initialized(bool initialized) { m_initialized = initialized; }

    bool dispatched() const { return m_dispatch; }
    void set_dispatched(bool dispatched) { m_dispatch = dispatched; }

    void prevent_default() { set_cancelled_flag(); }
    bool default_prevented() const { return cancelled(); }

    u16 event_phase() const { return m_phase; }
    void set_phase(Phase phase) { m_phase = phase; }

    JS::GCPtr<EventTarget> current_target() const { return m_current_target; }
    void set_current_target(EventTarget* current_target) { m_current_target = current_target; }

    bool return_value() const { return !m_cancelled; }
    void set_return_value(bool return_value)
    {
        if (!return_value)
            set_cancelled_flag();
    }

    void append_to_path(EventTarget&, JS::GCPtr<EventTarget>, JS::GCPtr<EventTarget>, TouchTargetList&, bool);
    Path& path() { return m_path; }
    Path const& path() const { return m_path; }
    void clear_path() { m_path.clear(); }

    void set_touch_target_list(TouchTargetList& touch_target_list) { m_touch_target_list = touch_target_list; }
    TouchTargetList& touch_target_list() { return m_touch_target_list; }
    void clear_touch_target_list() { m_touch_target_list.clear(); }

    bool bubbles() const { return m_bubbles; }
    void set_bubbles(bool bubbles) { m_bubbles = bubbles; }

    bool cancelable() const { return m_cancelable; }
    void set_cancelable(bool cancelable) { m_cancelable = cancelable; }

    bool is_trusted() const { return m_is_trusted; }
    void set_is_trusted(bool is_trusted) { m_is_trusted = is_trusted; }

    void stop_propagation() { m_stop_propagation = true; }

    bool cancel_bubble() const { return m_stop_propagation; }
    void set_cancel_bubble(bool cancel_bubble)
    {
        if (cancel_bubble)
            m_stop_propagation = true;
    }

    void stop_immediate_propagation()
    {
        m_stop_propagation = true;
        m_stop_immediate_propagation = true;
    }

    void init_event(String const&, bool, bool);

    void set_time_stamp(double time_stamp) { m_time_stamp = time_stamp; }

    Vector<JS::Handle<EventTarget>> composed_path() const;

    template<typename T>
    bool fast_is() const = delete;

    virtual bool is_mouse_event() const { return false; }
    virtual bool is_pointer_event() const { return false; }

protected:
    void initialize_event(String const&, bool, bool);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    FlyString m_type;
    JS::GCPtr<EventTarget> m_target;
    JS::GCPtr<EventTarget> m_related_target;
    JS::GCPtr<EventTarget> m_current_target;

    Phase m_phase { None };

    bool m_bubbles { false };
    bool m_cancelable { false };

    bool m_stop_propagation { false };
    bool m_stop_immediate_propagation { false };
    bool m_cancelled { false };
    bool m_in_passive_listener { false };
    bool m_composed { false };
    bool m_initialized { false };
    bool m_dispatch { false };

    bool m_is_trusted { false };

    Path m_path;
    TouchTargetList m_touch_target_list;

    double m_time_stamp { 0 };

    void set_cancelled_flag();
};

}
