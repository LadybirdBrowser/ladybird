/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/NavigateEventPrototype.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigateeventinit
struct NavigateEventInit : public DOM::EventInit {
    Bindings::NavigationType navigation_type = Bindings::NavigationType::Push;
    JS::GCPtr<NavigationDestination> destination;
    bool can_intercept = false;
    bool user_initiated = false;
    bool hash_change = false;
    JS::GCPtr<DOM::AbortSignal> signal;
    JS::GCPtr<XHR::FormData> form_data = nullptr;
    Optional<String> download_request = {};
    Optional<JS::Value> info;
    bool has_ua_visual_transition = false;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationintercepthandler
using NavigationInterceptHandler = JS::NonnullGCPtr<WebIDL::CallbackType>;

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationinterceptoptions
struct NavigationInterceptOptions {
    JS::GCPtr<WebIDL::CallbackType> handler;
    Optional<Bindings::NavigationFocusReset> focus_reset;
    Optional<Bindings::NavigationScrollBehavior> scroll;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigateevent
class NavigateEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(NavigateEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(NavigateEvent);

public:
    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-interception-state
    enum class InterceptionState {
        None,
        Intercepted,
        Committed,
        Scrolled,
        Finished
    };

    [[nodiscard]] static JS::NonnullGCPtr<NavigateEvent> construct_impl(JS::Realm&, FlyString const& event_name, NavigateEventInit const&);

    // The navigationType, destination, canIntercept, userInitiated, hashChange, signal, formData,
    // downloadRequest, info, and hasUAVisualTransition attributes must return the values they are initialized to.
    Bindings::NavigationType navigation_type() const { return m_navigation_type; }
    JS::NonnullGCPtr<NavigationDestination> destination() const { return m_destination; }
    bool can_intercept() const { return m_can_intercept; }
    bool user_initiated() const { return m_user_initiated; }
    bool hash_change() const { return m_hash_change; }
    JS::NonnullGCPtr<DOM::AbortSignal> signal() const { return m_signal; }
    JS::GCPtr<XHR::FormData> form_data() const { return m_form_data; }
    Optional<String> download_request() const { return m_download_request; }
    JS::Value info() const { return m_info; }
    bool has_ua_visual_transition() const { return m_has_ua_visual_transition; }

    WebIDL::ExceptionOr<void> intercept(NavigationInterceptOptions const&);
    WebIDL::ExceptionOr<void> scroll();

    virtual ~NavigateEvent() override;

    JS::NonnullGCPtr<DOM::AbortController> abort_controller() const { return *m_abort_controller; }
    InterceptionState interception_state() const { return m_interception_state; }
    Vector<NavigationInterceptHandler> const& navigation_handler_list() const { return m_navigation_handler_list; }
    Optional<SerializationRecord> classic_history_api_state() const { return m_classic_history_api_state; }

    void set_abort_controller(JS::NonnullGCPtr<DOM::AbortController> c) { m_abort_controller = c; }
    void set_interception_state(InterceptionState s) { m_interception_state = s; }
    void set_classic_history_api_state(Optional<SerializationRecord> r) { m_classic_history_api_state = move(r); }

    void finish(bool did_fulfill);

private:
    NavigateEvent(JS::Realm&, FlyString const& event_name, NavigateEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> perform_shared_checks();
    void process_scroll_behavior();
    void potentially_process_scroll_behavior();
    void potentially_reset_the_focus();

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-interception-state
    InterceptionState m_interception_state = InterceptionState::None;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-navigation-handler-list
    Vector<NavigationInterceptHandler> m_navigation_handler_list;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-focusreset
    Optional<Bindings::NavigationFocusReset> m_focus_reset_behavior = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-scroll
    Optional<Bindings::NavigationScrollBehavior> m_scroll_behavior = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-abort-controller
    JS::GCPtr<DOM::AbortController> m_abort_controller = { nullptr };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-classic-history-api-state
    Optional<SerializationRecord> m_classic_history_api_state = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-navigationtype
    Bindings::NavigationType m_navigation_type = { Bindings::NavigationType::Push };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-destination
    JS::NonnullGCPtr<NavigationDestination> m_destination;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-canintercept
    bool m_can_intercept = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-userinitiated
    bool m_user_initiated = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-hashchange
    bool m_hash_change = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-signal
    JS::NonnullGCPtr<DOM::AbortSignal> m_signal;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-formdata
    JS::GCPtr<XHR::FormData> m_form_data;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-downloadrequest
    Optional<String> m_download_request;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-info
    JS::Value m_info;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-hasuavisualtransition
    bool m_has_ua_visual_transition { false };
};

}

namespace AK {
template<>
struct Formatter<Web::Bindings::NavigationScrollBehavior> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Bindings::NavigationScrollBehavior const& value)
    {
        return Formatter<StringView>::format(builder, Web::Bindings::idl_enum_to_string(value));
    }
};

template<>
struct Formatter<Web::Bindings::NavigationFocusReset> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::Bindings::NavigationFocusReset const& value)
    {
        return Formatter<StringView>::format(builder, Web::Bindings::idl_enum_to_string(value));
    }
};
}
