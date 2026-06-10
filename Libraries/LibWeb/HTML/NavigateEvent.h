/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Types.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/NavigateEvent.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

struct NavigateEventInit;

}

namespace Web::DOM {

class AbortController;
class AbortSignal;
class Element;

}

namespace Web::XHR {

class FormData;

}

namespace Web::HTML {

class NavigationDestination;
class Window;

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationintercepthandler
using NavigationInterceptHandler = GC::Ref<WebIDL::CallbackType>;

using NavigationFocusReset = Bindings::NavigationFocusReset;
using NavigationScrollBehavior = Bindings::NavigationScrollBehavior;

struct NavigateEventInit : public DOM::EventInit {
    NavigationType navigation_type { NavigationType::Push };
    GC::Ref<NavigationDestination> destination;
    bool can_intercept { false };
    bool user_initiated { false };
    bool hash_change { false };
    GC::Ref<DOM::AbortSignal> signal;
    GC::Ptr<XHR::FormData> form_data;
    Optional<String> download_request;
    Optional<JS::Value> info;
    bool has_ua_visual_transition { false };
    GC::Ptr<DOM::Element> source_element;
};

using NavigationInterceptOptions = Bindings::NavigationInterceptOptions;

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigateevent
class NavigateEvent : public DOM::Event {
    WEB_WRAPPABLE(NavigateEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(NavigateEvent);

public:
    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-interception-state
    enum class InterceptionState {
        None,
        Intercepted,
        Committed,
        Scrolled,
        Finished
    };

    [[nodiscard]] static GC::Ref<NavigateEvent> create(JS::Realm&, FlyString const& event_name, Bindings::NavigateEventInit const&);
    [[nodiscard]] static GC::Ref<NavigateEvent> create(Window& relevant_window, FlyString const& event_name, NavigateEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    // The navigationType, destination, canIntercept, userInitiated, hashChange, signal, formData, downloadRequest,
    // info, hasUAVisualTransition, and sourceElement attributes must return the values they are initialized to.
    NavigationType navigation_type() const { return m_navigation_type; }
    GC::Ref<NavigationDestination> destination() const { return m_destination; }
    bool can_intercept() const { return m_can_intercept; }
    bool user_initiated() const { return m_user_initiated; }
    bool hash_change() const { return m_hash_change; }
    GC::Ref<DOM::AbortSignal> signal() const { return m_signal; }
    GC::Ptr<XHR::FormData> form_data() const { return m_form_data; }
    Optional<String> download_request() const { return m_download_request; }
    JS::Value const& info() const { return m_info; }
    bool has_ua_visual_transition() const { return m_has_ua_visual_transition; }
    GC::Ptr<DOM::Element> source_element() const { return m_source_element; }

    WebIDL::ExceptionOr<void> intercept(JS::Realm&, NavigationInterceptOptions const&);
    WebIDL::ExceptionOr<void> scroll();

    virtual ~NavigateEvent() override;

    Window& relevant_window() const { return *m_relevant_window; }

    GC::Ref<DOM::AbortController> abort_controller() const { return *m_abort_controller; }
    InterceptionState interception_state() const { return m_interception_state; }
    Vector<NavigationInterceptHandler> const& navigation_handler_list() const { return m_navigation_handler_list; }
    Optional<SerializationRecord> classic_history_api_state() const { return m_classic_history_api_state; }
    bool has_started_navigate_event_intercept_commit_handler_steps() const { return m_has_started_navigate_event_intercept_commit_handler_steps; }

    void set_abort_controller(GC::Ref<DOM::AbortController> c) { m_abort_controller = c; }
    void set_interception_state(InterceptionState s) { m_interception_state = s; }
    void set_classic_history_api_state(Optional<SerializationRecord> r) { m_classic_history_api_state = move(r); }
    void set_has_started_navigate_event_intercept_commit_handler_steps() { m_has_started_navigate_event_intercept_commit_handler_steps = true; }

    void finish(bool did_fulfill);

private:
    NavigateEvent(Window&, FlyString const& event_name, NavigateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> perform_shared_checks();
    void process_scroll_behavior();
    void potentially_process_scroll_behavior();
    void potentially_reset_the_focus();

    // https://html.spec.whatwg.org/multipage/webappapis.html#concept-relevant-global
    GC::Ref<Window> m_relevant_window;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-interception-state
    InterceptionState m_interception_state = InterceptionState::None;

    bool m_has_started_navigate_event_intercept_commit_handler_steps { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-navigation-handler-list
    Vector<NavigationInterceptHandler> m_navigation_handler_list;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-focusreset
    Optional<NavigationFocusReset> m_focus_reset_behavior = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-scroll
    Optional<NavigationScrollBehavior> m_scroll_behavior = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-abort-controller
    GC::Ptr<DOM::AbortController> m_abort_controller = { nullptr };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigateevent-classic-history-api-state
    Optional<SerializationRecord> m_classic_history_api_state = {};

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-navigationtype
    NavigationType m_navigation_type = { NavigationType::Push };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-destination
    GC::Ref<NavigationDestination> m_destination;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-canintercept
    bool m_can_intercept = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-userinitiated
    bool m_user_initiated = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-hashchange
    bool m_hash_change = { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-signal
    GC::Ref<DOM::AbortSignal> m_signal;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-formdata
    GC::Ptr<XHR::FormData> m_form_data;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-downloadrequest
    Optional<String> m_download_request;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-info
    JS::Value m_info;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-hasuavisualtransition
    bool m_has_ua_visual_transition { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigateevent-sourceelement
    GC::Ptr<DOM::Element> m_source_element { nullptr };
};

}
