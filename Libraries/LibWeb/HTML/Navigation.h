/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Promise.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::HTML {

class NavigationHistoryEntry;

using NavigationUpdateCurrentEntryOptions = Bindings::NavigationUpdateCurrentEntryOptions;
using NavigationOptions = Bindings::NavigationOptions;
using NavigationNavigateOptions = Bindings::NavigationNavigateOptions;
using NavigationReloadOptions = Bindings::NavigationReloadOptions;

struct NavigationResult {
    static NavigationResult from_promises(GC::Ref<WebIDL::Promise> committed, GC::Ref<WebIDL::Promise> finished)
    {
        return NavigationResult {
            .committed = committed,
            .finished = finished,
            .entry = nullptr,
        };
    }

    static NavigationResult resolved_with_entry(GC::Ref<NavigationHistoryEntry> entry)
    {
        return NavigationResult {
            .committed = nullptr,
            .finished = nullptr,
            .entry = entry,
        };
    }

    GC::Ptr<WebIDL::Promise> committed;
    GC::Ptr<WebIDL::Promise> finished;
    GC::Ptr<NavigationHistoryEntry> entry;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-method-tracker
struct NavigationAPIMethodTracker final : public JS::Cell {
    GC_CELL(NavigationAPIMethodTracker, JS::Cell);
    GC_DECLARE_ALLOCATOR(NavigationAPIMethodTracker);

    NavigationAPIMethodTracker(GC::Ref<Navigation> navigation,
        Optional<String> key,
        JS::Value info,
        Optional<SerializationRecord> serialized_state,
        GC::Ptr<NavigationHistoryEntry> committed_to_entry,
        GC::Ref<WebIDL::Promise> committed_promise,
        GC::Ref<WebIDL::Promise> finished_promise);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<Navigation> navigation;
    Optional<String> key;
    JS::Value info;
    Optional<SerializationRecord> serialized_state;
    GC::Ptr<NavigationHistoryEntry> committed_to_entry;
    GC::Ref<WebIDL::Promise> committed_promise;
    GC::Ref<WebIDL::Promise> finished_promise;
};

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-interface
class Navigation : public DOM::EventTarget {
    WEB_WRAPPABLE(Navigation, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(Navigation);

public:
    [[nodiscard]] static GC::Ref<Navigation> create(Window&);

    // IDL properties and methods
    Vector<GC::Ref<NavigationHistoryEntry>> entries() const;
    GC::Ptr<NavigationHistoryEntry> current_entry() const;
    WebIDL::ExceptionOr<void> update_current_entry(NavigationUpdateCurrentEntryOptions);

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-transition
    GC::Ptr<NavigationTransition> transition() const { return m_transition; }

    bool can_go_back() const;
    bool can_go_forward() const;

    WebIDL::ExceptionOr<Bindings::NavigationResult> navigate(JS::Realm&, String url, NavigationNavigateOptions const&);
    WebIDL::ExceptionOr<Bindings::NavigationResult> reload(JS::Realm&, NavigationReloadOptions const&);

    WebIDL::ExceptionOr<Bindings::NavigationResult> traverse_to(JS::Realm&, String key, NavigationOptions const&);
    WebIDL::ExceptionOr<Bindings::NavigationResult> back(JS::Realm&, NavigationOptions const&);
    WebIDL::ExceptionOr<Bindings::NavigationResult> forward(JS::Realm&, NavigationOptions const&);

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
    void abort_a_navigate_event(GC::Ref<NavigateEvent>, GC::Ref<WebIDL::DOMException> reason);
    bool fire_a_traverse_navigate_event(NonnullRefPtr<SessionHistoryEntry> destination_she, UserNavigationInvolvement = UserNavigationInvolvement::None);
    bool fire_a_push_replace_reload_navigate_event(
        NavigationType,
        URL::URL destination_url,
        bool is_same_document,
        UserNavigationInvolvement = UserNavigationInvolvement::None,
        GC::Ptr<DOM::Element> source_element = {},
        Optional<GC::ConservativeVector<XHR::FormDataEntry>&> form_data_entry_list = {},
        Optional<SerializationRecord> navigation_api_state = {},
        Optional<SerializationRecord> classic_history_api_state = {});
    bool fire_a_download_request_navigate_event(URL::URL destination_url, UserNavigationInvolvement user_involvement, GC::Ptr<DOM::Element> source_element, String filename);

    void initialize_the_navigation_api_entries_for_a_new_document(Vector<NonnullRefPtr<SessionHistoryEntry>> const& new_shes, NonnullRefPtr<SessionHistoryEntry> initial_she);
    void update_the_navigation_api_entries_for_a_same_document_navigation(NonnullRefPtr<SessionHistoryEntry> destination_she, NavigationType);

    virtual ~Navigation() override;

    // Internal Getters/Setters
    GC::Ptr<NavigateEvent> ongoing_navigate_event() const { return m_ongoing_navigate_event; }

    bool focus_changed_during_ongoing_navigation() const { return m_focus_changed_during_ongoing_navigation; }
    void set_focus_changed_during_ongoing_navigation(bool b) { m_focus_changed_during_ongoing_navigation = b; }

    void set_was_initial_about_blank_opened(bool b) { m_was_initial_about_blank_opened = b; }

private:
    explicit Navigation(Window&);

    Window& window() const;

    virtual void visit_edges(Visitor&) override;

    using AnyException = decltype(declval<WebIDL::ExceptionOr<void>>().exception());
    NavigationResult early_error_result(AnyException);
    NavigationResult early_error_result(GC::Ref<WebIDL::DOMException>);

    WebIDL::ExceptionOr<NavigationResult> navigate_internal(String url, NavigationNavigateOptions const&);
    WebIDL::ExceptionOr<NavigationResult> reload_internal(NavigationReloadOptions const&);
    WebIDL::ExceptionOr<NavigationResult> traverse_to_internal(String key, NavigationOptions const&);
    WebIDL::ExceptionOr<NavigationResult> back_internal(NavigationOptions const&);
    WebIDL::ExceptionOr<NavigationResult> forward_internal(NavigationOptions const&);

    GC::Ref<NavigationAPIMethodTracker> maybe_set_the_upcoming_non_traverse_api_method_tracker(JS::Value info, Optional<SerializationRecord>);
    GC::Ref<NavigationAPIMethodTracker> add_an_upcoming_traverse_api_method_tracker(String destination_key, JS::Value info);
    WebIDL::ExceptionOr<NavigationResult> perform_a_navigation_api_traversal(String key, NavigationOptions const&);
    void promote_an_upcoming_api_method_tracker_to_ongoing(Optional<String> destination_key);
    void resolve_the_finished_promise(GC::Ref<NavigationAPIMethodTracker>);
    void reject_the_finished_promise(GC::Ref<NavigationAPIMethodTracker>, JS::Value exception);
    void reject_the_finished_promise(GC::Ref<NavigationAPIMethodTracker>, GC::Ref<WebIDL::DOMException> exception);
    void clean_up(GC::Ref<NavigationAPIMethodTracker>);
    void notify_about_the_committed_to_entry(GC::Ref<NavigationAPIMethodTracker>, GC::Ref<NavigationHistoryEntry>);
    void run_the_navigate_event_intercept_commit_handler_steps(GC::Ref<NavigateEvent>, GC::Ptr<NavigationAPIMethodTracker>);

    bool inner_navigate_event_firing_algorithm(
        NavigationType,
        GC::Ref<NavigationDestination>,
        UserNavigationInvolvement,
        GC::Ptr<DOM::Element> source_element,
        Optional<GC::ConservativeVector<XHR::FormDataEntry>&> form_data_entry_list,
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

    GC::Ref<Window> m_window;

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

    // AD-HOC: Set when document.open() is called on an initial about:blank document.
    bool m_was_initial_about_blank_opened { false };
};

HistoryHandlingBehavior to_history_handling_behavior(NavigationHistoryBehavior);
NavigationHistoryBehavior to_navigation_history_behavior(HistoryHandlingBehavior);

}
