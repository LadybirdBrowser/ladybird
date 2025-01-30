/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Promise.h>
#include <LibWeb/Bindings/NavigationPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationupdatecurrententryoptions
struct NavigationUpdateCurrentEntryOptions {
    JS::Value state;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationoptions
struct NavigationOptions {
    Optional<JS::Value> info;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationnavigateoptions
struct NavigationNavigateOptions : public NavigationOptions {
    Optional<JS::Value> state;
    Bindings::NavigationHistoryBehavior history = Bindings::NavigationHistoryBehavior::Auto;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationreloadoptions
struct NavigationReloadOptions : public NavigationOptions {
    Optional<JS::Value> state;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationresult
struct NavigationResult {
    GC::Ref<WebIDL::Promise> committed;
    GC::Ref<WebIDL::Promise> finished;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-method-tracker
struct NavigationAPIMethodTracker final : public JS::Cell {
    GC_CELL(NavigationAPIMethodTracker, JS::Cell);
    GC_DECLARE_ALLOCATOR(NavigationAPIMethodTracker);

    NavigationAPIMethodTracker(GC::Ref<Navigation> navigation,
        Optional<String> key,
        JS::Value info,
        Optional<SerializationRecord> serialized_state,
        GC::Ptr<NavigationHistoryEntry> commited_to_entry,
        GC::Ref<WebIDL::Promise> committed_promise,
        GC::Ref<WebIDL::Promise> finished_promise);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Navigation> navigation;
    Optional<String> key;
    JS::Value info;
    Optional<SerializationRecord> serialized_state;
    GC::Ptr<NavigationHistoryEntry> commited_to_entry;
    GC::Ref<WebIDL::Promise> committed_promise;
    GC::Ref<WebIDL::Promise> finished_promise;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-interface
class Navigation : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(Navigation, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Navigation);

public:
    [[nodiscard]] static GC::Ref<Navigation> create(JS::Realm&);

    // IDL properties and methods
    Vector<GC::Ref<NavigationHistoryEntry>> entries() const;
    GC::Ptr<NavigationHistoryEntry> current_entry() const;
    WebIDL::ExceptionOr<void> update_current_entry(NavigationUpdateCurrentEntryOptions);

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-transition
    GC::Ptr<NavigationTransition> transition() const { return m_transition; }

    bool can_go_back() const;
    bool can_go_forward() const;

    WebIDL::ExceptionOr<NavigationResult> navigate(String url, NavigationNavigateOptions const&);
    WebIDL::ExceptionOr<NavigationResult> reload(NavigationReloadOptions const&);

    WebIDL::ExceptionOr<NavigationResult> traverse_to(String key, NavigationOptions const&);
    WebIDL::ExceptionOr<NavigationResult> back(NavigationOptions const&);
    WebIDL::ExceptionOr<NavigationResult> forward(NavigationOptions const&);

    // Event Handlers
    void set_onnavigate(WebIDL::CallbackType*);
    WebIDL::CallbackType* onnavigate();

    void set_onnavigatesuccess(WebIDL::CallbackType*);
    WebIDL::CallbackType* onnavigatesuccess();

    void set_onnavigateerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onnavigateerror();

    void set_oncurrententrychange(WebIDL::CallbackType*);
    WebIDL::CallbackType* oncurrententrychange();

    // Abstract Operations
    bool has_entries_and_events_disabled() const;
    i64 get_the_navigation_api_entry_index(SessionHistoryEntry const&) const;
    void abort_the_ongoing_navigation(GC::Ptr<WebIDL::DOMException> error = {});
    bool fire_a_traverse_navigate_event(GC::Ref<SessionHistoryEntry> destination_she, UserNavigationInvolvement = UserNavigationInvolvement::None);
    bool fire_a_push_replace_reload_navigate_event(
        Bindings::NavigationType,
        URL::URL destination_url,
        bool is_same_document,
        UserNavigationInvolvement = UserNavigationInvolvement::None,
        GC::Ptr<DOM::Element> source_element = {},
        Optional<Vector<XHR::FormDataEntry>&> form_data_entry_list = {},
        Optional<SerializationRecord> navigation_api_state = {},
        Optional<SerializationRecord> classic_history_api_state = {});
    bool fire_a_download_request_navigate_event(URL::URL destination_url, UserNavigationInvolvement user_involvement, GC::Ptr<DOM::Element> source_element, String filename);

    void initialize_the_navigation_api_entries_for_a_new_document(Vector<GC::Ref<SessionHistoryEntry>> const& new_shes, GC::Ref<SessionHistoryEntry> initial_she);
    void update_the_navigation_api_entries_for_a_same_document_navigation(GC::Ref<SessionHistoryEntry> destination_she, Bindings::NavigationType);

    virtual ~Navigation() override;

    // Internal Getters/Setters
    GC::Ptr<NavigateEvent> ongoing_navigate_event() const { return m_ongoing_navigate_event; }

    bool focus_changed_during_ongoing_navigation() const { return m_focus_changed_during_ongoing_navigation; }
    void set_focus_changed_during_ongoing_navigation(bool b) { m_focus_changed_during_ongoing_navigation = b; }

private:
    explicit Navigation(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    using AnyException = decltype(declval<WebIDL::ExceptionOr<void>>().exception());
    NavigationResult early_error_result(AnyException);

    GC::Ref<NavigationAPIMethodTracker> maybe_set_the_upcoming_non_traverse_api_method_tracker(JS::Value info, Optional<SerializationRecord>);
    GC::Ref<NavigationAPIMethodTracker> add_an_upcoming_traverse_api_method_tracker(String destination_key, JS::Value info);
    WebIDL::ExceptionOr<NavigationResult> perform_a_navigation_api_traversal(String key, NavigationOptions const&);
    void promote_an_upcoming_api_method_tracker_to_ongoing(Optional<String> destination_key);
    void resolve_the_finished_promise(GC::Ref<NavigationAPIMethodTracker>);
    void reject_the_finished_promise(GC::Ref<NavigationAPIMethodTracker>, JS::Value exception);
    void clean_up(GC::Ref<NavigationAPIMethodTracker>);
    void notify_about_the_committed_to_entry(GC::Ref<NavigationAPIMethodTracker>, GC::Ref<NavigationHistoryEntry>);

    bool inner_navigate_event_firing_algorithm(
        Bindings::NavigationType,
        GC::Ref<NavigationDestination>,
        UserNavigationInvolvement,
        GC::Ptr<DOM::Element> source_element,
        Optional<Vector<XHR::FormDataEntry>&> form_data_entry_list,
        Optional<String> download_request_filename,
        Optional<SerializationRecord> classic_history_api_state);

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-entry-list
    // Each Navigation has an associated entry list, a list of NavigationHistoryEntry objects, initially empty.
    Vector<GC::Ref<NavigationHistoryEntry>> m_entry_list;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-current-entry-index
    // Each Navigation has an associated current entry index, an integer, initially −1.
    i64 m_current_entry_index { -1 };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigation-transition
    // Each Navigation has a transition, which is a NavigationTransition or null, initially null.
    GC::Ptr<NavigationTransition> m_transition { nullptr };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#ongoing-navigate-event
    GC::Ptr<NavigateEvent> m_ongoing_navigate_event { nullptr };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#focus-changed-during-ongoing-navigation
    bool m_focus_changed_during_ongoing_navigation { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#suppress-normal-scroll-restoration-during-ongoing-navigation
    bool m_suppress_scroll_restoration_during_ongoing_navigation { false };

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#ongoing-api-method-tracker
    GC::Ptr<NavigationAPIMethodTracker> m_ongoing_api_method_tracker = nullptr;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#upcoming-non-traverse-api-method-tracker
    GC::Ptr<NavigationAPIMethodTracker> m_upcoming_non_traverse_api_method_tracker = nullptr;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#upcoming-non-traverse-api-method-tracker
    HashMap<String, GC::Ref<NavigationAPIMethodTracker>> m_upcoming_traverse_api_method_trackers;
};

HistoryHandlingBehavior to_history_handling_behavior(Bindings::NavigationHistoryBehavior);
Bindings::NavigationHistoryBehavior to_navigation_history_behavior(HistoryHandlingBehavior);

}
