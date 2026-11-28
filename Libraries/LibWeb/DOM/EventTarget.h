/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

// The bit of an event type in the listener type summaries of an event target's core (see Rust/src/dom/event_target.rs):
// one of 16, picked from the type's interned identity, which is the same for every equal type.
inline u16 event_type_summary_bit(Utf16FlyString const& type)
{
    return static_cast<u16>(1u << ((static_cast<u64>(type.raw_identity()) * 0x9E3779B97F4A7C15ull) >> 60));
}

class WEB_API EventTarget : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(EventTarget, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(EventTarget);

public:
    virtual ~EventTarget() override;

    static GC::Ref<EventTarget> create();

    virtual bool is_focusable() const { return false; }

    struct EventListenerOptions {
        bool capture { false };
    };

    struct AddEventListenerOptions {
        bool capture { false };
        Optional<bool> passive;
        bool once { false };
        GC::Ptr<AbortSignal> signal;
    };

    void add_event_listener(FlyString const& type, GC::Ptr<IDLEventListener> callback, AddEventListenerOptions const& options);
    void add_event_listener(Utf16FlyString const& type, GC::Ptr<IDLEventListener> callback, AddEventListenerOptions const& options);
    void remove_event_listener(FlyString const& type, GC::Ptr<IDLEventListener> callback, EventListenerOptions const& options);
    void remove_event_listener(Utf16FlyString const& type, GC::Ptr<IDLEventListener> callback, EventListenerOptions const& options);

    // NOTE: These are for internal use only. They operate as though addEventListener(type, callback) was called instead of addEventListener(type, callback, options).
    void add_event_listener_without_options(Utf16FlyString const& type, IDLEventListener& callback);
    void remove_event_listener_without_options(Utf16FlyString const& type, IDLEventListener& callback);

    virtual bool dispatch_event(Event&);
    WebIDL::ExceptionOr<bool> dispatch_event_for_bindings(Event&);

    virtual EventTarget* get_parent(Event const&) { return nullptr; }

    void add_an_event_listener(DOMEventListener&);
    void remove_an_event_listener(DOMEventListener&);
    void remove_all_event_listeners();
    void erase_all_event_listeners_and_handlers();

    using ClonedEventListeners = GC::RootVector<GC::Ref<DOMEventListener>, 4>;
    ClonedEventListeners event_listener_list_matching(Utf16FlyString const& type) const;

    // The steps that follow from removing listener from the event listener list, see remove_an_event_listener().
    void run_event_listener_removal_steps(DOMEventListener const& listener);

    virtual bool has_activation_behavior() const;
    virtual void activation_behavior(Event const&);

    // NOTE: These only exist for checkbox and radio input elements.
    virtual void legacy_pre_activation_behavior() { }
    virtual void legacy_cancelled_activation_behavior() { }
    virtual void legacy_cancelled_activation_behavior_was_not_called() { }

    WebIDL::CallbackType* event_handler_attribute(Utf16FlyString const& name);
    void set_event_handler_attribute(Utf16FlyString const& name, GC::Ptr<WebIDL::CallbackType>);

    bool has_event_listener(Utf16FlyString const& type) const;
    bool has_blocking_wheel_event_listener() const;
    bool has_blocking_keyboard_event_listener() const;
    bool has_event_listeners() const { return (m_event_target_core.capture_listener_types | m_event_target_core.bubble_listener_types) != 0; }

    // Whether the target may have a listener of any of the types whose bits are set in types, see
    // event_type_summary_bit(). A type whose bit is clear has none.
    bool may_have_event_listener(u16 types) const { return ((m_event_target_core.capture_listener_types | m_event_target_core.bubble_listener_types) & types) != 0; }

protected:
    EventTarget();

    // Says what the target's class is, which its constructor does. See the EVENT_TARGET_* flags.
    void set_event_target_core_flag(u8 flag) { m_event_target_core.flags |= flag; }

    void element_event_handler_attribute_changed(Utf16FlyString const& local_name, Optional<Utf16String> const& value);
    virtual void event_listener_list_changed() { }

    virtual void visit_edges(Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

private:
    struct Data {
        AK_ALLOC_WITH_KMALLOC;

        ~Data();

        // https://dom.spec.whatwg.org/#eventtarget-event-listener-list
        // NB: libweb_rust defines its layout, and adds and removes the listeners (see Rust/src/dom/event_listeners.rs).
        //     It finds the list at the start of the data.
        RustFFI::EventListenerList event_listener_list { nullptr, 0, 0 };

        // https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-map
        // Spec Note: The order of the entries of event handler map could be arbitrary. It is not observable through any algorithms that operate on the map.
        HashMap<Utf16FlyString, GC::Ref<HTML::EventHandler>> event_handler_map;
    };

    Data& ensure_data();
    ReadonlySpan<GC::Member<DOMEventListener>> event_listeners() const;

    template<typename>
    friend struct GC::CellConstructionHook;

    // NB: The core comes first, so that it fills the padding at the end of the cell's header.
    RustFFI::EventTargetCore m_event_target_core {};
    OwnPtr<Data> m_data;

    WebIDL::CallbackType* get_current_value_of_event_handler(Utf16FlyString const& name);
    void activate_event_handler(Utf16FlyString const& name, HTML::EventHandler& event_handler);
    void deactivate_event_handler(Utf16FlyString const& name);
    JS::ThrowCompletionOr<void> process_event_handler_for_event(Utf16FlyString const& name, Event& event);
};

bool is_window_reflecting_body_element_event_handler(Utf16FlyString const& name);

}

namespace GC {

// Tells libweb_rust, as an event target that is no node is constructed, whether its class has a "get the parent" of its
// own (see Rust/src/dom/event_target.rs). DOM::Node says what a node's class has in a hook of its own.
template<typename T>
requires(IsBaseOf<Web::DOM::EventTarget, T> && !IsBaseOf<Web::DOM::Node, T>)
struct CellConstructionHook<T> {
    static void did_construct(T& target)
    {
        using Web::DOM::Event;
        using Web::DOM::EventTarget;
        // NB: The dispatch in libweb_rust asks only nodes whether they have activation behavior.
        static_assert(IsSame<decltype(&T::has_activation_behavior), bool (EventTarget::*)() const>);
        if constexpr (!IsSame<decltype(&T::get_parent), EventTarget* (EventTarget::*)(Event const&)>)
            target.set_event_target_core_flag(Web::DOM::RustFFI::EVENT_TARGET_HAS_GET_THE_PARENT);
    }
};

}
