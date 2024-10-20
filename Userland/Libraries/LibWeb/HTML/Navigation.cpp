/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigationPrototype.h>
#include <LibWeb/DOM/AbortController.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/NavigateEvent.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationCurrentEntryChangeEvent.h>
#include <LibWeb/HTML/NavigationDestination.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/NavigationTransition.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/XHR/FormData.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(Navigation);
JS_DEFINE_ALLOCATOR(NavigationAPIMethodTracker);

static NavigationResult navigation_api_method_tracker_derived_result(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker);

NavigationAPIMethodTracker::NavigationAPIMethodTracker(JS::NonnullGCPtr<Navigation> navigation,
    Optional<String> key,
    JS::Value info,
    Optional<SerializationRecord> serialized_state,
    JS::GCPtr<NavigationHistoryEntry> commited_to_entry,
    JS::NonnullGCPtr<WebIDL::Promise> committed_promise,
    JS::NonnullGCPtr<WebIDL::Promise> finished_promise)
    : navigation(navigation)
    , key(move(key))
    , info(info)
    , serialized_state(move(serialized_state))
    , commited_to_entry(commited_to_entry)
    , committed_promise(committed_promise)
    , finished_promise(finished_promise)
{
}

void NavigationAPIMethodTracker::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(navigation);
    visitor.visit(info);
    visitor.visit(commited_to_entry);
    visitor.visit(committed_promise);
    visitor.visit(finished_promise);
}

JS::NonnullGCPtr<Navigation> Navigation::create(JS::Realm& realm)
{
    return realm.heap().allocate<Navigation>(realm, realm);
}

Navigation::Navigation(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

Navigation::~Navigation() = default;

void Navigation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Navigation);
}

void Navigation::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_entry_list);
    visitor.visit(m_transition);
    visitor.visit(m_ongoing_navigate_event);
    visitor.visit(m_ongoing_api_method_tracker);
    visitor.visit(m_upcoming_non_traverse_api_method_tracker);
    visitor.visit(m_upcoming_traverse_api_method_trackers);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-entries
Vector<JS::NonnullGCPtr<NavigationHistoryEntry>> Navigation::entries() const
{
    // The entries() method steps are:

    // 1. If this has entries and events disabled, then return the empty list.
    if (has_entries_and_events_disabled())
        return {};

    // 2. Return this's entry list.
    //    NOTE: Recall that because of Web IDL's sequence type conversion rules,
    //          this will create a new JavaScript array object on each call.
    //          That is, navigation.entries() !== navigation.entries().
    return m_entry_list;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-current-entry
JS::GCPtr<NavigationHistoryEntry> Navigation::current_entry() const
{
    // The current entry of a Navigation navigation is the result of running the following steps:

    // 1. If navigation has entries and events disabled, then return null.
    if (has_entries_and_events_disabled())
        return nullptr;

    // 2. Assert: navigation's current entry index is not −1.
    VERIFY(m_current_entry_index != -1);

    // 3. Return navigation's entry list[navigation's current entry index].
    return m_entry_list[m_current_entry_index];
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-updatecurrententry
WebIDL::ExceptionOr<void> Navigation::update_current_entry(NavigationUpdateCurrentEntryOptions options)
{
    // The updateCurrentEntry(options) method steps are:

    // 1. Let current be the current entry of this.
    auto current = current_entry();

    // 2. If current is null, then throw an "InvalidStateError" DOMException.
    if (current == nullptr)
        return WebIDL::InvalidStateError::create(realm(), "Cannot update current NavigationHistoryEntry when there is no current entry"_string);

    // 3. Let serializedState be StructuredSerializeForStorage(options["state"]), rethrowing any exceptions.
    auto serialized_state = TRY(structured_serialize_for_storage(vm(), options.state));

    // 4. Set current's session history entry's navigation API state to serializedState.
    current->session_history_entry().set_navigation_api_state(serialized_state);

    // 5. Fire an event named currententrychange at this using NavigationCurrentEntryChangeEvent,
    //    with its navigationType attribute initialized to null and its from initialized to current.
    NavigationCurrentEntryChangeEventInit event_init = {};
    event_init.navigation_type = {};
    event_init.from = current;
    dispatch_event(HTML::NavigationCurrentEntryChangeEvent::construct_impl(realm(), HTML::EventNames::currententrychange, event_init));

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-cangoback
bool Navigation::can_go_back() const
{
    // The canGoBack getter steps are:

    // 1. If this has entries and events disabled, then return false.
    if (has_entries_and_events_disabled())
        return false;

    // 2. Assert: navigation's current entry index is not −1.
    VERIFY(m_current_entry_index != -1);

    // 3. If this's current entry index is 0, then return false.
    // 4. Return true.
    return (m_current_entry_index != 0);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-cangoforward
bool Navigation::can_go_forward() const
{
    // The canGoForward getter steps are:

    // 1. If this has entries and events disabled, then return false.
    if (has_entries_and_events_disabled())
        return false;

    // 2. Assert: navigation's current entry index is not −1.
    VERIFY(m_current_entry_index != -1);

    // 3. If this's current entry index is equal to this's entry list's size, then return false.
    // 4. Return true.
    return (m_current_entry_index != static_cast<i64>(m_entry_list.size()));
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#history-handling-behavior
HistoryHandlingBehavior to_history_handling_behavior(Bindings::NavigationHistoryBehavior b)
{
    // A history handling behavior is a NavigationHistoryBehavior that is either "push" or "replace",
    // i.e., that has been resolved away from any initial "auto" value.
    VERIFY(b != Bindings::NavigationHistoryBehavior::Auto);

    switch (b) {
    case Bindings::NavigationHistoryBehavior::Push:
        return HistoryHandlingBehavior::Push;
    case Bindings::NavigationHistoryBehavior::Replace:
        return HistoryHandlingBehavior::Replace;
    case Bindings::NavigationHistoryBehavior::Auto:
        VERIFY_NOT_REACHED();
    };
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#history-handling-behavior
Bindings::NavigationHistoryBehavior to_navigation_history_behavior(HistoryHandlingBehavior b)
{
    // A history handling behavior is a NavigationHistoryBehavior that is either "push" or "replace",
    // i.e., that has been resolved away from any initial "auto" value.

    switch (b) {
    case HistoryHandlingBehavior::Push:
        return Bindings::NavigationHistoryBehavior::Push;
    case HistoryHandlingBehavior::Replace:
        return Bindings::NavigationHistoryBehavior::Replace;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-navigate
WebIDL::ExceptionOr<NavigationResult> Navigation::navigate(String url, NavigationNavigateOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // The navigate(options) method steps are:

    // 1. Parse url relative to this's relevant settings object.
    //    If that returns failure, then return an early error result for a "SyntaxError" DOMException.
    //    Otherwise, let urlRecord be the resulting URL record.
    auto url_record = relevant_settings_object(*this).parse_url(url);
    if (!url_record.is_valid())
        return early_error_result(WebIDL::SyntaxError::create(realm, "Cannot navigate to Invalid URL"_string));

    // 2. Let document be this's relevant global object's associated Document.
    auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 3. If options["history"] is "push", and the navigation must be a replace given urlRecord and document,
    //    then return an early error result for a "NotSupportedError" DOMException.
    if (options.history == Bindings::NavigationHistoryBehavior::Push && navigation_must_be_a_replace(url_record, document))
        return early_error_result(WebIDL::NotSupportedError::create(realm, "Navigation must be a replace, but push was requested"_string));

    // 4. Let state be options["state"], if it exists; otherwise, undefined.
    auto state = options.state.value_or(JS::js_undefined());

    // 5. Let serializedState be StructuredSerializeForStorage(state).
    //    If this throws an exception, then return an early error result for that exception.
    // FIXME: Fix this spec grammaro in the note
    // NOTE: It is importantly to perform this step early, since serialization can invoke web developer code,
    //       which in turn might change various things we check in later steps.
    auto serialized_state_or_error = structured_serialize_for_storage(vm, state);
    if (serialized_state_or_error.is_error()) {
        return early_error_result(serialized_state_or_error.release_error());
    }

    auto serialized_state = serialized_state_or_error.release_value();

    // 6. If document is not fully active, then return an early error result for an "InvalidStateError" DOMException.
    if (!document.is_fully_active())
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string));

    // 7. If document's unload counter is greater than 0, then return an early error result for an "InvalidStateError" DOMException.
    if (document.unload_counter() > 0)
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document already unloaded"_string));

    // 8. Let info be options["info"], if it exists; otherwise, undefined.
    auto info = options.info.value_or(JS::js_undefined());

    // 9. Let apiMethodTracker be the result of maybe setting the upcoming non-traverse API method tracker for this
    //    given info and serializedState.
    auto api_method_tracker = maybe_set_the_upcoming_non_traverse_api_method_tracker(info, serialized_state);

    // 10. Navigate document's node navigable to urlRecord using document,
    //     with historyHandling set to options["history"] and navigationAPIState set to serializedState.
    // FIXME: Fix spec typo here
    // NOTE: Unlike location.assign() and friends, which are exposed across origin-domain boundaries,
    //       navigation.navigate() can only be accessed by code with direct synchronous access to the
    //       window.navigation property. Thus, we avoid the complications about attributing the source document
    //       of the navigation, and we don't need to deal with the allowed by sandboxing to navigate check and its
    //       acccompanying exceptionsEnabled flag. We just treat all navigations as if they come from the Document
    //       corresponding to this Navigation object itself (i.e., document).
    TRY(document.navigable()->navigate({ .url = url_record, .source_document = document, .history_handling = options.history, .navigation_api_state = move(serialized_state) }));

    // 11. If this's upcoming non-traverse API method tracker is apiMethodTracker, then:
    // NOTE: If the upcoming non-traverse API method tracker is still apiMethodTracker, this means that the navigate
    //       algorithm bailed out before ever getting to the inner navigate event firing algorithm which would promote
    //       that upcoming API method tracker to ongoing.
    if (m_upcoming_non_traverse_api_method_tracker == api_method_tracker) {
        m_upcoming_non_traverse_api_method_tracker = nullptr;
        return early_error_result(WebIDL::AbortError::create(realm, "Navigation aborted"_string));
    }

    // 12. Return a navigation API method tracker-derived result for apiMethodTracker.
    return navigation_api_method_tracker_derived_result(api_method_tracker);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-reload
WebIDL::ExceptionOr<NavigationResult> Navigation::reload(NavigationReloadOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();
    // The reload(options) method steps are:

    // 1. Let document be this's relevant global object's associated Document.
    auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 2. Let serializedState be StructuredSerializeForStorage(undefined).
    auto serialized_state = MUST(structured_serialize_for_storage(vm, JS::js_undefined()));

    // 3. If options["state"] exists, then set serializedState to StructuredSerializeForStorage(options["state"]).
    //    If this throws an exception, then return an early error result for that exception.
    // NOTE: It is importantly to perform this step early, since serialization can invoke web developer
    //       code, which in turn might change various things we check in later steps.
    if (options.state.has_value()) {
        auto serialized_state_or_error = structured_serialize_for_storage(vm, options.state.value());
        if (serialized_state_or_error.is_error())
            return early_error_result(serialized_state_or_error.release_error());
        serialized_state = serialized_state_or_error.release_value();
    }

    // 4. Otherwise:
    else {
        // 1. Let current be the current entry of this.
        auto current = current_entry();

        // 2. If current is not null, then set serializedState to current's session history entry's navigation API state.
        if (current != nullptr)
            serialized_state = current->session_history_entry().navigation_api_state();
    }

    // 5. If document is not fully active, then return an early error result for an "InvalidStateError" DOMException.
    if (!document.is_fully_active())
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string));

    // 6. If document's unload counter is greater than 0, then return an early error result for an "InvalidStateError" DOMException.
    if (document.unload_counter() > 0)
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document already unloaded"_string));

    // 7. Let info be options["info"], if it exists; otherwise, undefined.
    auto info = options.info.value_or(JS::js_undefined());

    // 8. Let apiMethodTracker be the result of maybe setting the upcoming non-traverse API method tracker for this given info and serializedState.
    auto api_method_tracker = maybe_set_the_upcoming_non_traverse_api_method_tracker(info, serialized_state);

    // 9. Reload document's node navigable with navigationAPIState set to serializedState.
    // FIXME: Pass serialized_state to reload
    document.navigable()->reload();

    return navigation_api_method_tracker_derived_result(api_method_tracker);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-traverseto
WebIDL::ExceptionOr<NavigationResult> Navigation::traverse_to(String key, NavigationOptions const& options)
{
    auto& realm = this->realm();
    // The traverseTo(key, options) method steps are:

    // 1. If this's current entry index is −1, then return an early error result for an "InvalidStateError" DOMException.
    if (m_current_entry_index == -1)
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Cannot traverseTo: no current session history entry"_string));

    // 2. If this's entry list does not contain a NavigationHistoryEntry whose session history entry's navigation API key equals key,
    //    then return an early error result for an "InvalidStateError" DOMException.
    auto it = m_entry_list.find_if([&key](auto const& entry) {
        return entry->session_history_entry().navigation_api_key() == key;
    });
    if (it == m_entry_list.end())
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Cannot traverseTo: key not found in session history list"_string));

    // 3. Return the result of performing a navigation API traversal given this, key, and options.
    return perform_a_navigation_api_traversal(key, options);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
WebIDL::ExceptionOr<NavigationResult> Navigation::back(NavigationOptions const& options)
{
    auto& realm = this->realm();
    // The back(options) method steps are:

    // 1. If this's current entry index is −1 or 0, then return an early error result for an "InvalidStateError" DOMException.
    if (m_current_entry_index == -1 || m_current_entry_index == 0)
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Cannot navigate back: no previous session history entry"_string));

    // 2. Let key be this's entry list[this's current entry index − 1]'s session history entry's navigation API key.
    auto key = m_entry_list[m_current_entry_index - 1]->session_history_entry().navigation_api_key();

    // 3. Return the result of performing a navigation API traversal given this, key, and options.
    return perform_a_navigation_api_traversal(key, options);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigation-forward
WebIDL::ExceptionOr<NavigationResult> Navigation::forward(NavigationOptions const& options)
{
    auto& realm = this->realm();
    // The forward(options) method steps are:

    // 1. If this's current entry index is −1 or is equal to this's entry list's size − 1,
    //    then return an early error result for an "InvalidStateError" DOMException.
    if (m_current_entry_index == -1 || m_current_entry_index == static_cast<i64>(m_entry_list.size() - 1))
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Cannot navigate forward: no next session history entry"_string));

    // 2. Let key be this's entry list[this's current entry index + 1]'s session history entry's navigation API key.
    auto key = m_entry_list[m_current_entry_index + 1]->session_history_entry().navigation_api_key();

    // 3. Return the result of performing a navigation API traversal given this, key, and options.
    return perform_a_navigation_api_traversal(key, options);
}

void Navigation::set_onnavigate(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::navigate, event_handler);
}

WebIDL::CallbackType* Navigation::onnavigate()
{
    return event_handler_attribute(HTML::EventNames::navigate);
}

void Navigation::set_onnavigatesuccess(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::navigatesuccess, event_handler);
}

WebIDL::CallbackType* Navigation::onnavigatesuccess()
{
    return event_handler_attribute(HTML::EventNames::navigatesuccess);
}

void Navigation::set_onnavigateerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::navigateerror, event_handler);
}

WebIDL::CallbackType* Navigation::onnavigateerror()
{
    return event_handler_attribute(HTML::EventNames::navigateerror);
}

void Navigation::set_oncurrententrychange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::currententrychange, event_handler);
}

WebIDL::CallbackType* Navigation::oncurrententrychange()
{
    return event_handler_attribute(HTML::EventNames::currententrychange);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#has-entries-and-events-disabled
bool Navigation::has_entries_and_events_disabled() const
{
    // A Navigation navigation has entries and events disabled if the following steps return true:

    // 1. Let document be navigation's relevant global object's associated Document.
    auto const& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 2. If document is not fully active, then return true.
    if (!document.is_fully_active())
        return true;

    // 3. If document's is initial about:blank is true, then return true.
    if (document.is_initial_about_blank())
        return true;

    // 4. If document's origin is opaque, then return true.
    if (document.origin().is_opaque())
        return true;

    // 5. Return false.
    return false;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#getting-the-navigation-api-entry-index
i64 Navigation::get_the_navigation_api_entry_index(SessionHistoryEntry const& she) const
{
    // To get the navigation API entry index of a session history entry she within a Navigation navigation:

    // 1. Let index be 0.
    i64 index = 0;

    // 2. For each nhe of navigation's entry list:
    for (auto const& nhe : m_entry_list) {
        // 1. If nhe's session history entry is equal to she, then return index.
        if (&nhe->session_history_entry() == &she)
            return index;

        // 2. Increment index by 1.
        ++index;
    }

    // 3. Return −1.
    return -1;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-early-error-result
NavigationResult Navigation::early_error_result(AnyException e)
{
    auto& vm = this->vm();

    // An early error result for an exception e is a NavigationResult dictionary instance given by
    // «[ "committed" → a promise rejected with e, "finished" → a promise rejected with e ]».
    auto throw_completion = Bindings::dom_exception_to_throw_completion(vm, e);
    return {
        .committed = WebIDL::create_rejected_promise(realm(), *throw_completion.value())->promise(),
        .finished = WebIDL::create_rejected_promise(realm(), *throw_completion.value())->promise(),
    };
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-method-tracker-derived-result
NavigationResult navigation_api_method_tracker_derived_result(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker)
{
    // A navigation API method tracker-derived result for a navigation API method tracker is a NavigationResult
    /// dictionary instance given by «[ "committed" apiMethodTracker's committed promise, "finished" → apiMethodTracker's finished promise ]».
    return {
        api_method_tracker->committed_promise->promise(),
        api_method_tracker->finished_promise->promise(),
    };
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#upcoming-non-traverse-api-method-tracker
JS::NonnullGCPtr<NavigationAPIMethodTracker> Navigation::maybe_set_the_upcoming_non_traverse_api_method_tracker(JS::Value info, Optional<SerializationRecord> serialized_state)
{
    auto& realm = relevant_realm(*this);
    auto& vm = this->vm();
    // To maybe set the upcoming non-traverse API method tracker given a Navigation navigation,
    // a JavaScript value info, and a serialized state-or-null serializedState:

    // 1. Let committedPromise and finishedPromise be new promises created in navigation's relevant realm.
    auto committed_promise = WebIDL::create_promise(realm);
    auto finished_promise = WebIDL::create_promise(realm);

    // 2. Mark as handled finishedPromise.
    // NOTE: The web developer doesn’t necessarily care about finishedPromise being rejected:
    //       - They might only care about committedPromise.
    //       - They could be doing multiple synchronous navigations within the same task,
    //         in which case all but the last will be aborted (causing their finishedPromise to reject).
    //         This could be an application bug, but also could just be an emergent feature of disparate
    //         parts of the application overriding each others' actions.
    //       - They might prefer to listen to other transition-failure signals instead of finishedPromise, e.g.,
    //         the navigateerror event, or the navigation.transition.finished promise.
    //       As such, we mark it as handled to ensure that it never triggers unhandledrejection events.
    WebIDL::mark_promise_as_handled(finished_promise);

    // 3. Let apiMethodTracker be a new navigation API method tracker with:
    //     navigation object: navigation
    //     key:               null
    //     info:              info
    //     serialized state:  serializedState
    //     comitted-to entry: null
    //     comitted promise:  committedPromise
    //     finished promise:  finishedPromise
    auto api_method_tracker = vm.heap().allocate_without_realm<NavigationAPIMethodTracker>(
        /* .navigation = */ *this,
        /* .key = */ OptionalNone {},
        /* .info = */ info,
        /* .serialized_state = */ move(serialized_state),
        /* .commited_to_entry = */ nullptr,
        /* .committed_promise = */ committed_promise,
        /* .finished_promise = */ finished_promise);

    // 4. Assert: navigation's upcoming non-traverse API method tracker is null.
    VERIFY(m_upcoming_non_traverse_api_method_tracker == nullptr);

    // 5. If navigation does not have entries and events disabled,
    //    then set navigation's upcoming non-traverse API method tracker to apiMethodTracker.
    // NOTE: If navigation has entries and events disabled, then committedPromise and finishedPromise will never fulfill
    //      (since we never create a NavigationHistoryEntry object for such Documents, and so we have nothing to resolve them with);
    //      there is no NavigationHistoryEntry to apply serializedState to; and there is no navigate event to include info with.
    //      So, we don't need to track this API method call after all.
    if (!has_entries_and_events_disabled())
        m_upcoming_non_traverse_api_method_tracker = api_method_tracker;

    // 6. Return apiMethodTracker.
    return api_method_tracker;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#add-an-upcoming-traverse-api-method-tracker
JS::NonnullGCPtr<NavigationAPIMethodTracker> Navigation::add_an_upcoming_traverse_api_method_tracker(String destination_key, JS::Value info)
{
    auto& vm = this->vm();
    auto& realm = relevant_realm(*this);
    // To add an upcoming traverse API method tracker given a Navigation navigation, a string destinationKey, and a JavaScript value info:

    // 1. Let committedPromise and finishedPromise be new promises created in navigation's relevant realm.
    auto committed_promise = WebIDL::create_promise(realm);
    auto finished_promise = WebIDL::create_promise(realm);

    // 2. Mark as handled finishedPromise.
    // NOTE: See the previous discussion about why this is done
    //       https://html.spec.whatwg.org/multipage/nav-history-apis.html#note-mark-as-handled-navigation-api-finished
    WebIDL::mark_promise_as_handled(*finished_promise);

    // 3. Let apiMethodTracker be a new navigation API method tracker with:
    //     navigation object: navigation
    //     key:               destinationKey
    //     info:              info
    //     serialized state:  null
    //     comitted-to entry: null
    //     comitted promise:  committedPromise
    //     finished promise:  finishedPromise
    auto api_method_tracker = vm.heap().allocate_without_realm<NavigationAPIMethodTracker>(
        /* .navigation = */ *this,
        /* .key = */ destination_key,
        /* .info = */ info,
        /* .serialized_state = */ OptionalNone {},
        /* .commited_to_entry = */ nullptr,
        /* .committed_promise = */ committed_promise,
        /* .finished_promise = */ finished_promise);

    // 4. Set navigation's upcoming traverse API method trackers[key] to apiMethodTracker.
    // FIXME: Fix spec typo key --> destinationKey
    m_upcoming_traverse_api_method_trackers.set(destination_key, api_method_tracker);

    // 5. Return apiMethodTracker.
    return api_method_tracker;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#performing-a-navigation-api-traversal
WebIDL::ExceptionOr<NavigationResult> Navigation::perform_a_navigation_api_traversal(String key, NavigationOptions const& options)
{
    auto& realm = this->realm();
    // To perform a navigation API traversal given a Navigation navigation, a string key, and a NavigationOptions options:

    // 1. Let document be this's relevant global object's associated Document.
    auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 2. If document is not fully active, then return an early error result for an "InvalidStateError" DOMException.
    if (!document.is_fully_active())
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document is not fully active"_string));

    // 3. If document's unload counter is greater than 0, then return an early error result for an "InvalidStateError" DOMException.
    if (document.unload_counter() > 0)
        return early_error_result(WebIDL::InvalidStateError::create(realm, "Document already unloaded"_string));

    // 4. Let current be the current entry of navigation.
    auto current = current_entry();

    // 5. If key equals current's session history entry's navigation API key, then return
    //    «[ "committed" → a promise resolved with current, "finished" → a promise resolved with current ]».
    if (key == current->session_history_entry().navigation_api_key()) {
        return NavigationResult {
            .committed = WebIDL::create_resolved_promise(realm, current)->promise(),
            .finished = WebIDL::create_resolved_promise(realm, current)->promise()
        };
    }

    // 6. If navigation's upcoming traverse API method trackers[key] exists,
    //    then return a navigation API method tracker-derived result for navigation's upcoming traverse API method trackers[key].
    if (auto maybe_tracker = m_upcoming_traverse_api_method_trackers.get(key); maybe_tracker.has_value())
        return navigation_api_method_tracker_derived_result(maybe_tracker.value());

    // 7. Let info be options["info"], if it exists; otherwise, undefined.
    auto info = options.info.value_or(JS::js_undefined());

    // 8. Let apiMethodTracker be the result of adding an upcoming traverse API method tracker for navigation given key and info.
    auto api_method_tracker = add_an_upcoming_traverse_api_method_tracker(key, info);

    // 9. Let navigable be document's node navigable.
    auto navigable = document.navigable();

    // 10. Let traversable be navigable's traversable navigable.
    auto traversable = navigable->traversable_navigable();

    // 11. Let sourceSnapshotParams be the result of snapshotting source snapshot params given document.
    auto source_snapshot_params = document.snapshot_source_snapshot_params();

    // 12. Append the following session history traversal steps to traversable:
    traversable->append_session_history_traversal_steps(JS::create_heap_function(heap(), [key, api_method_tracker, navigable, source_snapshot_params, traversable, this] {
        // 1. Let navigableSHEs be the result of getting session history entries given navigable.
        auto navigable_shes = navigable->get_session_history_entries();

        // 2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no such entry exists, then:
        auto it = navigable_shes.find_if([&key](auto const& entry) {
            return entry->navigation_api_key() == key;
        });
        if (it == navigable_shes.end()) {
            // NOTE: This path is taken if navigation's entry list was outdated compared to navigableSHEs,
            //       which can occur for brief periods while all the relevant threads and processes are being synchronized in reaction to a history change.

            // 1. Queue a global task on the navigation and traversal task source given navigation's relevant global object
            //    to reject the finished promise for apiMethodTracker with an "InvalidStateError" DOMException.
            queue_global_task(HTML::Task::Source::NavigationAndTraversal, relevant_global_object(*this), JS::create_heap_function(heap(), [this, api_method_tracker] {
                auto& reject_realm = relevant_realm(*this);
                TemporaryExecutionContext execution_context { relevant_settings_object(*this) };
                WebIDL::reject_promise(reject_realm, api_method_tracker->finished_promise,
                    WebIDL::InvalidStateError::create(reject_realm, "Cannot traverse with stale session history entry"_string));
            }));

            // 2. Abort these steps.
            return;
        }
        auto target_she = *it;

        // 3. If targetSHE is navigable's active session history entry, then abort these steps.
        // NOTE: This can occur if a previously queued traversal already took us to this session history entry.
        //       In that case the previous traversal will have dealt with apiMethodTracker already.
        if (target_she == navigable->active_session_history_entry())
            return;

        // 4. Let result be the result of applying the traverse history step given by targetSHE's step to traversable,
        //    given sourceSnapshotParams, navigable, and "none".
        auto result = traversable->apply_the_traverse_history_step(target_she->step().get<int>(), source_snapshot_params, navigable, UserNavigationInvolvement::None);

        // NOTE: When result is "canceled-by-beforeunload" or "initiator-disallowed", the navigate event was never fired,
        //       aborting the ongoing navigation would not be correct; it would result in a navigateerror event without a
        //       preceding navigate event. In the "canceled-by-navigate" case, navigate is fired, but the inner navigate event
        //       firing algorithm will take care of aborting the ongoing navigation.

        // 5. If result is "canceled-by-beforeunload", then queue a global task on the navigation and traversal task source
        //    given navigation's relevant global object to reject the finished promise for apiMethodTracker with a
        //    new "AbortError" DOMException created in navigation's relevant realm.
        auto& realm = relevant_realm(*this);
        auto& global = relevant_global_object(*this);
        if (result == TraversableNavigable::HistoryStepResult::CanceledByBeforeUnload) {
            queue_global_task(Task::Source::NavigationAndTraversal, global, JS::create_heap_function(heap(), [this, api_method_tracker, &realm] {
                TemporaryExecutionContext execution_context { relevant_settings_object(*this) };
                reject_the_finished_promise(api_method_tracker, WebIDL::AbortError::create(realm, "Navigation cancelled by beforeunload"_string));
            }));
        }

        // 6. If result is "initiator-disallowed", then queue a global task on the navigation and traversal task source
        //    given navigation's relevant global object to reject the finished promise for apiMethodTracker with a
        //    new "SecurityError" DOMException created in navigation's relevant realm.
        if (result == TraversableNavigable::HistoryStepResult::InitiatorDisallowed) {
            queue_global_task(Task::Source::NavigationAndTraversal, global, JS::create_heap_function(heap(), [this, api_method_tracker, &realm] {
                TemporaryExecutionContext execution_context { relevant_settings_object(*this) };
                reject_the_finished_promise(api_method_tracker, WebIDL::SecurityError::create(realm, "Navigation disallowed from this origin"_string));
            }));
        }
    }));

    // 13. Return a navigation API method tracker-derived result for apiMethodTracker.
    return navigation_api_method_tracker_derived_result(api_method_tracker);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#abort-the-ongoing-navigation
void Navigation::abort_the_ongoing_navigation(JS::GCPtr<WebIDL::DOMException> error)
{
    auto& realm = relevant_realm(*this);

    // To abort the ongoing navigation given a Navigation navigation and an optional DOMException error:

    // 1. Let event be navigation's ongoing navigate event.
    auto event = ongoing_navigate_event();

    // 2. Assert: event is not null.
    VERIFY(event != nullptr);

    // 3. Set navigation's focus changed during ongoing navigation to false.
    m_focus_changed_during_ongoing_navigation = false;

    // 4. Set navigation's suppress normal scroll restoration during ongoing navigation to false.
    m_suppress_scroll_restoration_during_ongoing_navigation = false;

    // 5. If error was not given, then let error be a new "AbortError" DOMException created in navigation's relevant realm.
    if (!error)
        error = WebIDL::AbortError::create(realm, "Navigation aborted"_string);

    VERIFY(error);

    // 6. If event's dispatch flag is set, then set event's canceled flag to true.
    if (event->dispatched())
        event->set_cancelled(true);

    // 7. Signal abort on event's abort controller given error.
    event->abort_controller()->abort(error);

    // 8. Set navigation's ongoing navigate event to null.
    m_ongoing_navigate_event = nullptr;

    // 9. Fire an event named navigateerror at navigation using ErrorEvent, with error initialized to error,
    //   and message, filename, lineno, and colno initialized to appropriate values that can be extracted
    //   from error and the current JavaScript stack in the same underspecified way that the report the exception algorithm does.
    ErrorEventInit event_init = {};
    event_init.error = error;
    // FIXME: Extract information from the exception and the JS context in the wishy-washy way the spec says here.
    event_init.filename = String {};
    event_init.colno = 0;
    event_init.lineno = 0;
    event_init.message = String {};

    dispatch_event(ErrorEvent::create(realm, EventNames::navigateerror, event_init));

    // 10. If navigation's ongoing API method tracker is non-null, then reject the finished promise for apiMethodTracker with error.
    if (m_ongoing_api_method_tracker != nullptr)
        WebIDL::reject_promise(realm, m_ongoing_api_method_tracker->finished_promise, error);

    // 11. If navigation's transition is not null, then:
    if (m_transition != nullptr) {
        // 1. Reject navigation's transition's finished promise with error.
        m_transition->finished()->reject(error);

        // 2. Set navigation's transition to null.
        m_transition = nullptr;
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#promote-an-upcoming-api-method-tracker-to-ongoing
void Navigation::promote_an_upcoming_api_method_tracker_to_ongoing(Optional<String> destination_key)
{
    // 1. Assert: navigation's ongoing API method tracker is null.
    VERIFY(m_ongoing_api_method_tracker == nullptr);

    // 2. If destinationKey is not null, then:
    if (destination_key.has_value()) {
        // 1. Assert: navigation's upcoming non-traverse API method tracker is null.
        VERIFY(m_upcoming_non_traverse_api_method_tracker == nullptr);

        // 2. If navigation's upcoming traverse API method trackers[destinationKey] exists, then:
        if (auto tracker = m_upcoming_traverse_api_method_trackers.get(destination_key.value()); tracker.has_value()) {
            // 1. Set navigation's ongoing API method tracker to navigation's upcoming traverse API method trackers[destinationKey].
            m_ongoing_api_method_tracker = tracker.value();

            // 2. Remove navigation's upcoming traverse API method trackers[destinationKey].
            m_upcoming_traverse_api_method_trackers.remove(destination_key.value());
        }
    }

    // 3. Otherwise:
    else {
        // 1. Set navigation's ongoing API method tracker to navigation's upcoming non-traverse API method tracker.
        m_ongoing_api_method_tracker = m_upcoming_non_traverse_api_method_tracker;

        // 2. Set navigation's upcoming non-traverse API method tracker to null.
        m_upcoming_non_traverse_api_method_tracker = nullptr;
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-api-method-tracker-clean-up
void Navigation::clean_up(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker)
{
    // 1. Let navigation be apiMethodTracker's navigation object.
    VERIFY(api_method_tracker->navigation == this);

    // 2. If navigation's ongoing API method tracker is apiMethodTracker, then set navigation's ongoing API method tracker to null.
    if (m_ongoing_api_method_tracker == api_method_tracker) {
        m_ongoing_api_method_tracker = nullptr;
    }
    // 3. Otherwise:
    else {
        // 1. Let key be apiMethodTracker's key.
        auto& key = api_method_tracker->key;

        // 2. Assert: key is not null.
        VERIFY(key.has_value());

        // 3. Assert: navigation's upcoming traverse API method trackers[key] exists.
        VERIFY(m_upcoming_traverse_api_method_trackers.contains(*key));

        // 4. Remove navigation's upcoming traverse API method trackers[key].
        m_upcoming_traverse_api_method_trackers.remove(*key);
    }
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#resolve-the-finished-promise
void Navigation::resolve_the_finished_promise(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker)
{
    auto& realm = this->realm();

    // 1. Resolve apiMethodTracker's committed promise with its committed-to entry.
    // NOTE: Usually, notify about the committed-to entry has previously been called on apiMethodTracker,
    //       and so this will do nothing. However, in some cases resolve the finished promise is called
    //       directly, in which case this step is necessary.
    WebIDL::resolve_promise(realm, api_method_tracker->committed_promise, api_method_tracker->commited_to_entry);

    // 2. Resolve apiMethodTracker's finished promise with its committed-to entry.
    WebIDL::resolve_promise(realm, api_method_tracker->finished_promise, api_method_tracker->commited_to_entry);

    // 3. Clean up apiMethodTracker.
    clean_up(api_method_tracker);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#reject-the-finished-promise
void Navigation::reject_the_finished_promise(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker, JS::Value exception)
{
    auto& realm = this->realm();

    // 1. Reject apiMethodTracker's committed promise with exception.
    // NOTE: This will do nothing if apiMethodTracker's committed promise was previously resolved
    //       via notify about the committed-to entry.
    WebIDL::reject_promise(realm, api_method_tracker->committed_promise, exception);

    // 2. Reject apiMethodTracker's finished promise with exception.
    WebIDL::reject_promise(realm, api_method_tracker->finished_promise, exception);

    // 3. Clean up apiMethodTracker.
    clean_up(api_method_tracker);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#notify-about-the-committed-to-entry
void Navigation::notify_about_the_committed_to_entry(JS::NonnullGCPtr<NavigationAPIMethodTracker> api_method_tracker, JS::NonnullGCPtr<NavigationHistoryEntry> nhe)
{
    auto& realm = this->realm();

    // 1. Set apiMethodTracker's committed-to entry to nhe.
    api_method_tracker->commited_to_entry = nhe;

    // 2. If apiMethodTracker's serialized state is not null, then set nhe's session history entry's navigation API state to apiMethodTracker's serialized state.'
    // NOTE: If it's null, then we're traversing to nhe via navigation.traverseTo(), which does not allow changing the state.
    if (api_method_tracker->serialized_state.has_value()) {
        // NOTE: At this point, apiMethodTracker's serialized state is no longer needed.
        //       Implementations might want to clear it out to avoid keeping it alive for the lifetime of the navigation API method tracker.
        nhe->session_history_entry().set_navigation_api_state(*api_method_tracker->serialized_state);
        api_method_tracker->serialized_state = {};
    }

    // 3. Resolve apiMethodTracker's committed promise with nhe.
    TemporaryExecutionContext execution_context { relevant_settings_object(*this) };
    WebIDL::resolve_promise(realm, api_method_tracker->committed_promise, nhe);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#inner-navigate-event-firing-algorithm
bool Navigation::inner_navigate_event_firing_algorithm(
    Bindings::NavigationType navigation_type,
    JS::NonnullGCPtr<NavigationDestination> destination,
    UserNavigationInvolvement user_involvement,
    Optional<Vector<XHR::FormDataEntry>&> form_data_entry_list,
    Optional<String> download_request_filename,
    Optional<SerializationRecord> classic_history_api_state)
{
    // NOTE: Specification assumes that ongoing navigation event is cancelled before dispatching next navigation event.
    if (m_ongoing_navigate_event)
        abort_the_ongoing_navigation();

    auto& realm = relevant_realm(*this);

    // 1. If navigation has entries and events disabled, then:
    // NOTE: These assertions holds because traverseTo(), back(), and forward() will immediately fail when entries and events are disabled
    //       (since there are no entries to traverse to), and if our starting point is instead navigate() or reload(),
    //       then we avoided setting the upcoming non-traverse API method tracker in the first place.
    if (has_entries_and_events_disabled()) {
        // 1. Assert: navigation's ongoing API method tracker is null.
        VERIFY(m_ongoing_api_method_tracker == nullptr);

        // 2. Assert: navigation's upcoming non-traverse API method tracker is null.
        VERIFY(m_upcoming_non_traverse_api_method_tracker == nullptr);

        // 3. Assert: navigation's upcoming traverse API method trackers is empty.
        VERIFY(m_upcoming_traverse_api_method_trackers.is_empty());

        // 4. Return true.
        return true;
    }

    // 2. Let destinationKey be null.
    Optional<String> destination_key = {};

    // 3. If destination's entry is non-null, then set destinationKey to destination's entry's key.
    if (destination->navigation_history_entry() != nullptr)
        destination_key = destination->navigation_history_entry()->key();

    // 4. Assert: destinationKey is not the empty string.
    VERIFY(destination_key != ""sv);

    // 5. Promote an upcoming API method tracker to ongoing given navigation and destinationKey.
    promote_an_upcoming_api_method_tracker_to_ongoing(destination_key);

    // 6. Let apiMethodTracker be navigation's ongoing API method tracker.
    auto api_method_tracker = m_ongoing_api_method_tracker;

    // 7. Let navigable be navigation's relevant global object's navigable.
    auto& relevant_global_object = verify_cast<HTML::Window>(Web::HTML::relevant_global_object(*this));
    auto navigable = relevant_global_object.navigable();

    // 8. Let document be navigation's relevant global object's associated Document.
    auto& document = relevant_global_object.associated_document();

    // Note: We create the Event in this algorithm instead of passing it in,
    //       and have all the following "initialize" steps set up the event init
    NavigateEventInit event_init = {};

    // 9.  If document can have its URL rewritten to destination's URL,
    //     and either destination's is same document is true or navigationType is not "traverse",
    //     then initialize event's canIntercept to true. Otherwise, initialize it to false.
    event_init.can_intercept = can_have_its_url_rewritten(document, destination->raw_url()) && (destination->same_document() || navigation_type != Bindings::NavigationType::Traverse);

    // 10. Let traverseCanBeCanceled be true if all of the following are true:
    //      - navigable is a top-level traversable;
    //      - destination's is same document is true; and
    //      - either userInvolvement is not "browser UI", or navigation's relevant global object has history-action activation.
    //     Otherwise, let it be false.
    bool const traverse_can_be_canceled = navigable->is_top_level_traversable()
        && destination->same_document()
        && (user_involvement != UserNavigationInvolvement::BrowserUI || relevant_global_object.has_history_action_activation());

    // 11. If either:
    //      - navigationType is not "traverse"; or
    //      - traverseCanBeCanceled is true
    //     then initialize event's cancelable to true. Otherwise, initialize it to false.
    event_init.cancelable = (navigation_type != Bindings::NavigationType::Traverse) || traverse_can_be_canceled;

    // 12. Initialize event's type to "navigate".
    // AD-HOC: Happens later, when calling the factory function

    // 13. Initialize event's navigationType to navigationType.
    event_init.navigation_type = navigation_type;

    // 14. Initialize event's destination to destination.
    event_init.destination = destination;

    // 15. Initialize event's downloadRequest to downloadRequestFilename.
    event_init.download_request = move(download_request_filename);

    // 16. If apiMethodTracker is not null, then initialize event's info to apiMethodTracker's info. Otherwise, initialize it to undefined.
    // NOTE: At this point apiMethodTracker's info is no longer needed and can be nulled out instead of keeping it alive for the lifetime of the navigation API method tracker.
    if (api_method_tracker) {
        event_init.info = api_method_tracker->info;
        api_method_tracker->info = JS::js_undefined();
    } else {
        event_init.info = JS::js_undefined();
    }

    // FIXME: 17: Initialize event's hasUAVisualTransition to true if a visual transition, to display a cached rendered state
    //     of the document's latest entry, was done by the user agent. Otherwise, initialize it to false.
    event_init.has_ua_visual_transition = false;

    // 18. Set event's abort controller to a new AbortController created in navigation's relevant realm.
    // AD-HOC: Set on the NavigateEvent later after construction
    auto abort_controller = MUST(DOM::AbortController::construct_impl(realm));

    // 19. Initialize event's signal to event's abort controller's signal.
    event_init.signal = abort_controller->signal();

    // 20. Let currentURL be document's URL.
    auto current_url = document.url();

    // 21. If all of the following are true:
    //  - event's classic history API state is null;
    //  - destination's is same document is true;
    //  - destination's URL equals currentURL with exclude fragments set to true; and
    //  - destination's URL's fragment is not identical to currentURL's fragment,
    //  then initialize event's hashChange to true. Otherwise, initialize it to false.
    event_init.hash_change = (!classic_history_api_state.has_value()
        && destination->same_document()
        && destination->raw_url().equals(current_url, URL::ExcludeFragment::Yes)
        && destination->raw_url().fragment() != current_url.fragment());

    // 22. If userInvolvement is not "none", then initialize event's userInitiated to true. Otherwise, initialize it to false.
    event_init.user_initiated = user_involvement != UserNavigationInvolvement::None;

    // 23. If formDataEntryList is not null, then initialize event's formData to a new FormData created in navigation's relevant realm,
    //     associated to formDataEntryList. Otherwise, initialize it to null.
    if (form_data_entry_list.has_value()) {
        event_init.form_data = MUST(XHR::FormData::construct_impl(realm, form_data_entry_list.release_value()));
    } else {
        event_init.form_data = nullptr;
    }

    // AD-HOC: *Now* we have all the info required to create the event
    auto event = NavigateEvent::construct_impl(realm, EventNames::navigate, event_init);
    event->set_abort_controller(abort_controller);

    // AD-HOC: This is supposed to be set in "fire a <type> navigate event", and is only non-null when
    //         we're doing a push or replace. We set it here because we create the event here
    event->set_classic_history_api_state(move(classic_history_api_state));

    // 24. Assert: navigation's ongoing navigate event is null.
    VERIFY(m_ongoing_navigate_event == nullptr);

    // 25. Set navigation's ongoing navigate event to event.
    m_ongoing_navigate_event = event;

    // 26. Set navigation's focus changed during ongoing navigation to false.
    m_focus_changed_during_ongoing_navigation = false;

    // 27. Set navigation's suppress normal scroll restoration during ongoing navigation to false.
    m_suppress_scroll_restoration_during_ongoing_navigation = false;

    // 28. Let dispatchResult be the result of dispatching event at navigation.
    auto dispatch_result = dispatch_event(*event);

    // 29. If dispatchResult is false:
    if (!dispatch_result) {
        // 1. If navigationType is "traverse", then consume history-action user activation given navigation's relevant global object.
        if (navigation_type == Bindings::NavigationType::Traverse)
            relevant_global_object.consume_history_action_user_activation();

        // 2. If event's abort controller's signal is not aborted, then abort the ongoing navigation given navigation.
        if (!event->abort_controller()->signal()->aborted())
            abort_the_ongoing_navigation();

        // 3. Return false.
        return false;
    }

    // 30. Let endResultIsSameDocument be true if event's interception state
    //     is not "none" or event's destination's is same document is true.
    bool const end_result_is_same_document = (event->interception_state() != NavigateEvent::InterceptionState::None) || event->destination()->same_document();

    // 31. Prepare to run script given navigation's relevant settings object.
    // NOTE: There's a massive spec note here
    TemporaryExecutionContext execution_context { relevant_settings_object(*this), TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 32. If event's interception state is not "none":
    if (event->interception_state() != NavigateEvent::InterceptionState::None) {
        // 1. Set event's interception state to "committed".
        event->set_interception_state(NavigateEvent::InterceptionState::Committed);

        // 2. Let fromNHE be the current entry of navigation.
        auto from_nhe = current_entry();

        // 3. Assert: fromNHE is not null.
        VERIFY(from_nhe != nullptr);

        // 4. Set navigation's transition to a new NavigationTransition created in navigation's relevant realm,
        //    whose navigation type is navigationType, from entry is fromNHE, and whose finished promise is a new promise
        //    created in navigation's relevant realm.
        m_transition = NavigationTransition::create(realm, navigation_type, *from_nhe, JS::Promise::create(realm));

        // 5. Mark as handled navigation's transition's finished promise.
        m_transition->finished()->set_is_handled();

        // 6. If navigationType is "traverse", then set navigation's suppress normal scroll restoration during ongoing navigation to true.
        // NOTE: If event's scroll behavior was set to "after-transition", then scroll restoration will happen as part of finishing
        //       the relevant NavigateEvent. Otherwise, there will be no scroll restoration. That is, no navigation which is intercepted
        //       by intercept() goes through the normal scroll restoration process; scroll restoration for such navigations
        //       is either done manually, by the web developer, or is done after the transition.
        if (navigation_type == Bindings::NavigationType::Traverse)
            m_suppress_scroll_restoration_during_ongoing_navigation = true;

        // FIXME: Fix spec typo "serialied"
        // 7. If navigationType is "push" or "replace", then run the URL and history update steps given document and
        //    event's destination's URL, with serialiedData set to event's classic history API state and historyHandling
        //    set to navigationType.
        if (navigation_type == Bindings::NavigationType::Push || navigation_type == Bindings::NavigationType::Replace) {
            auto history_handling = navigation_type == Bindings::NavigationType::Push ? HistoryHandlingBehavior::Push : HistoryHandlingBehavior::Replace;
            perform_url_and_history_update_steps(document, event->destination()->raw_url(), event->classic_history_api_state(), history_handling);
        }
        // Big spec note about reload here
    }

    // 33. If endResultIsSameDocument is true:
    if (end_result_is_same_document) {
        // 1. Let promisesList be an empty list.
        JS::MarkedVector<JS::NonnullGCPtr<WebIDL::Promise>> promises_list(realm.heap());

        // 2. For each handler of event's navigation handler list:
        for (auto const& handler : event->navigation_handler_list()) {
            // 1. Append the result of invoking handler with an empty arguments list to promisesList.
            auto result = WebIDL::invoke_callback(handler, {});
            // This *should* be equivalent to converting a promise to a promise capability
            promises_list.append(WebIDL::create_resolved_promise(realm, result.value().value()));
        }

        // 3. If promisesList's size is 0, then set promisesList to « a promise resolved with undefined ».
        // NOTE: There is a subtle timing difference between how waiting for all schedules its success and failure
        //       steps when given zero promises versus ≥1 promises. For most uses of waiting for all, this does not matter.
        //       However, with this API, there are so many events and promise handlers which could fire around the same time
        //       that the difference is pretty easily observable: it can cause the event/promise handler sequence to vary.
        //       (Some of the events and promises involved include: navigatesuccess / navigateerror, currententrychange,
        //       dispose, apiMethodTracker's promises, and the navigation.transition.finished promise.)
        if (promises_list.size() == 0) {
            promises_list.append(WebIDL::create_resolved_promise(realm, JS::js_undefined()));
        }

        // 4. Wait for all of promisesList, with the following success steps:
        WebIDL::wait_for_all(
            realm, promises_list, [event, this, api_method_tracker](auto const&) -> void {

                // FIXME: Spec issue: Event's relevant global objects' *associated document*
                // 1. If event's relevant global object is not fully active, then abort these steps.
                auto& relevant_global_object = verify_cast<HTML::Window>(HTML::relevant_global_object(*event));
                auto& realm = event->realm();
                if (!relevant_global_object.associated_document().is_fully_active())
                    return;

                // 2. If event's abort controller's signal is aborted, then abort these steps.
                if (event->abort_controller()->signal()->aborted())
                    return;

                // 3. Assert: event equals navigation's ongoing navigate event.
                VERIFY(event == m_ongoing_navigate_event);

                // 4. Set navigation's ongoing navigate event to null.
                m_ongoing_navigate_event = nullptr;

                // 5. Finish event given true.
                event->finish(true);

                // FIXME: Implement https://dom.spec.whatwg.org/#concept-event-fire somewhere
                // 6. Fire an event named navigatesuccess at navigation.
                dispatch_event(DOM::Event::create(realm, EventNames::navigatesuccess));

                // 7. If apiMethodTracker is non-null, then resolve the finished promise for apiMethodTracker.
                if (api_method_tracker != nullptr)
                    resolve_the_finished_promise(*api_method_tracker);

                // 8. If navigation's transition is not null, then resolve navigation's transition's finished promise with undefined.
                if (m_transition != nullptr)
                    m_transition->finished()->fulfill(JS::js_undefined());

                // 9. Set navigation's transition to null.
                m_transition = nullptr; },
            // and the following failure steps given reason rejectionReason:
            [event, this, api_method_tracker](JS::Value rejection_reason) -> void {
                // FIXME: Spec issue: Event's relevant global objects' *associated document*
                // 1. If event's relevant global object is not fully active, then abort these steps.
                auto& relevant_global_object = verify_cast<HTML::Window>(HTML::relevant_global_object(*event));
                auto& realm = event->realm();
                if (!relevant_global_object.associated_document().is_fully_active())
                    return;

                // 2. If event's abort controller's signal is aborted, then abort these steps.
                if (event->abort_controller()->signal()->aborted())
                    return;

                // 3. Assert: event equals navigation's ongoing navigate event.
                VERIFY(event == m_ongoing_navigate_event);

                // 4. Set navigation's ongoing navigate event to null.
                m_ongoing_navigate_event = nullptr;

                // 5. Finish event given false.
                event->finish(false);

                // 6. Let errorInfo be the result of extracting error information from rejectionReason.
                ErrorEventInit event_init = {};
                event_init.error = rejection_reason;
                // FIXME: Extract information from the exception and the JS context in the wishy-washy way the spec says here.
                event_init.filename = String {};
                event_init.colno = 0;
                event_init.lineno = 0;
                event_init.message = String {};

                // 7. Fire an event named navigateerror at navigation using ErrorEvent,with additional attributes initialized according to errorInfo.
                dispatch_event(ErrorEvent::create(realm, EventNames::navigateerror, event_init));

                // 8. If apiMethodTracker is non-null, then reject the finished promise for apiMethodTracker with rejectionReason.
                if (api_method_tracker != nullptr)
                    reject_the_finished_promise(*api_method_tracker, rejection_reason);

                // 9. If navigation's transition is not null, then reject navigation's transition's finished promise with rejectionReason.
                if (m_transition)
                    m_transition->finished()->reject(rejection_reason);

                // 10. Set navigation's transition to null.
                m_transition = nullptr;
            });
    }

    // 34. Otherwise, if apiMethodTracker is non-null, then clean up apiMethodTracker.
    else if (api_method_tracker != nullptr) {
        clean_up(*api_method_tracker);
    }

    // 35. Clean up after running script given navigation's relevant settings object.
    // Handled by TemporaryExecutionContext destructor from step 31

    // 36. If event's interception state is "none", then return true.
    // 37. Return false.
    return event->interception_state() == NavigateEvent::InterceptionState::None;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#fire-a-traverse-navigate-event
bool Navigation::fire_a_traverse_navigate_event(JS::NonnullGCPtr<SessionHistoryEntry> destination_she, UserNavigationInvolvement user_involvement)
{
    auto& realm = relevant_realm(*this);
    auto& vm = this->vm();

    // 1. Let event be the result of creating an event given NavigateEvent, in navigation's relevant realm.
    // 2. Set event's classic history API state to null.
    // AD-HOC: These are handled in the inner algorithm

    // 3. Let destination be a new NavigationDestination created in navigation's relevant realm.
    auto destination = NavigationDestination::create(realm);

    // 4. Set destination's URL to destinationSHE's URL.
    destination->set_url(destination_she->url());

    // 5. Let destinationNHE be the NavigationHistoryEntry in navigation's entry list whose session history entry is destinationSHE,
    //    or null if no such NavigationHistoryEntry exists.
    auto destination_nhe = m_entry_list.find_if([destination_she](auto& nhe) {
        return &nhe->session_history_entry() == destination_she;
    });

    // 6. If destinationNHE is non-null, then:
    if (destination_nhe != m_entry_list.end()) {
        // 1. Set destination's entry to destinationNHE.
        destination->set_entry(*destination_nhe);

        // 2. Set destination's state to destinationSHE's navigation API state.
        destination->set_state(destination_she->navigation_api_state());
    }

    // 7. Otherwise:
    else {
        // 1. Set destination's entry to null.
        destination->set_entry(nullptr);

        // 2. Set destination's state to StructuredSerializeForStorage(null).
        destination->set_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));
    }

    // 8. Set destination's is same document to true if destinationSHE's document is equal to
    //    navigation's relevant global object's associated Document; otherwise false.
    destination->set_is_same_document(destination_she->document() == &verify_cast<Window>(relevant_global_object(*this)).associated_document());

    // 9. Return the result of performing the inner navigate event firing algorithm given navigation, "traverse", event, destination, userInvolvement, null, and null.
    // AD-HOC: We don't pass the event, but we do pass the classic_history_api state at the end to be set later
    return inner_navigate_event_firing_algorithm(Bindings::NavigationType::Traverse, destination, user_involvement, {}, {}, {});
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#fire-a-push/replace/reload-navigate-event
bool Navigation::fire_a_push_replace_reload_navigate_event(
    Bindings::NavigationType navigation_type,
    URL::URL destination_url,
    bool is_same_document,
    UserNavigationInvolvement user_involvement,
    Optional<Vector<XHR::FormDataEntry>&> form_data_entry_list,
    Optional<SerializationRecord> navigation_api_state,
    Optional<SerializationRecord> classic_history_api_state)
{
    auto& realm = relevant_realm(*this);
    auto& vm = this->vm();

    // This fulfills the entry requirement: an optional serialized state navigationAPIState (default StructuredSerializeForStorage(null))
    if (!navigation_api_state.has_value())
        navigation_api_state = MUST(structured_serialize_for_storage(vm, JS::js_null()));

    // 1. Let event be the result of creating an event given NavigateEvent, in navigation's relevant realm.
    // 2. Set event's classic history API state to classicHistoryAPIState.
    // AD-HOC: These are handled in the inner algorithm

    // 3. Let destination be a new NavigationDestination created in navigation's relevant realm.
    auto destination = NavigationDestination::create(realm);

    // 4. Set destination's URL to destinationURL.
    destination->set_url(destination_url);

    // 5. Set destination's entry to null.
    destination->set_entry(nullptr);

    // 6. Set destination's state to navigationAPIState.
    destination->set_state(*navigation_api_state);

    // 7. Set destination's is same document to isSameDocument.
    destination->set_is_same_document(is_same_document);

    // 8. Return the result of performing the inner navigate event firing algorithm given navigation,
    //    navigationType, event, destination, userInvolvement, formDataEntryList, and null.
    // AD-HOC: We don't pass the event, but we do pass the classic_history_api state at the end to be set later
    return inner_navigate_event_firing_algorithm(navigation_type, destination, user_involvement, move(form_data_entry_list), {}, move(classic_history_api_state));
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#fire-a-download-request-navigate-event
bool Navigation::fire_a_download_request_navigate_event(URL::URL destination_url, UserNavigationInvolvement user_involvement, String filename)
{
    auto& realm = relevant_realm(*this);
    auto& vm = this->vm();

    // 1. Let event be the result of creating an event given NavigateEvent, in navigation's relevant realm.
    // 2. Set event's classic history API state to classicHistoryAPIState.
    // AD-HOC: These are handled in the inner algorithm

    // 3. Let destination be a new NavigationDestination created in navigation's relevant realm.
    auto destination = NavigationDestination::create(realm);

    // 4. Set destination's URL to destinationURL.
    destination->set_url(destination_url);

    // 5. Set destination's entry to null.
    destination->set_entry(nullptr);

    // 6. Set destination's state to StructuredSerializeForStorage(null).
    destination->set_state(MUST(structured_serialize_for_storage(vm, JS::js_null())));

    // 7. Set destination's is same document to false.
    destination->set_is_same_document(false);

    // 8. Return the result of performing the inner navigate event firing algorithm given navigation,
    //   "push", event, destination, userInvolvement, null, and filename.
    // AD-HOC: We don't pass the event, but we do pass the classic_history_api state at the end to be set later
    return inner_navigate_event_firing_algorithm(Bindings::NavigationType::Push, destination, user_involvement, {}, move(filename), {});
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#initialize-the-navigation-api-entries-for-a-new-document
void Navigation::initialize_the_navigation_api_entries_for_a_new_document(Vector<JS::NonnullGCPtr<SessionHistoryEntry>> const& new_shes, JS::NonnullGCPtr<SessionHistoryEntry> initial_she)
{
    auto& realm = relevant_realm(*this);

    // 1. Assert: navigation's entry list is empty.
    VERIFY(m_entry_list.is_empty());

    // 2. Assert: navigation's current entry index is −1.
    VERIFY(m_current_entry_index == -1);

    // 3. If navigation has entries and events disabled, then return.
    if (has_entries_and_events_disabled())
        return;

    // 4. For each newSHE of newSHEs:
    for (auto const& new_she : new_shes) {
        // 1. Let newNHE be a new NavigationHistoryEntry created in the relevant realm of navigation.
        // 2. Set newNHE's session history entry to newSHE.
        auto new_nhe = NavigationHistoryEntry::create(realm, new_she);

        // 3. Append newNHE to navigation's entry list.
        m_entry_list.append(new_nhe);
    }

    // 5. Set navigation's current entry index to the result of getting the navigation API entry index of initialSHE within navigation.
    m_current_entry_index = get_the_navigation_api_entry_index(*initial_she);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#update-the-navigation-api-entries-for-a-same-document-navigation
void Navigation::update_the_navigation_api_entries_for_a_same_document_navigation(JS::NonnullGCPtr<SessionHistoryEntry> destination_she, Bindings::NavigationType navigation_type)
{
    auto& realm = relevant_realm(*this);

    // 1. If navigation has entries and events disabled, then return.
    if (has_entries_and_events_disabled())
        return;

    // 2. Let oldCurrentNHE be the current entry of navigation.
    auto old_current_nhe = current_entry();

    // 3. Let disposedNHEs be a new empty list.
    Vector<JS::NonnullGCPtr<NavigationHistoryEntry>> disposed_nhes;

    // 4. If navigationType is "traverse", then:
    if (navigation_type == Bindings::NavigationType::Traverse) {
        // 1. Set navigation's current entry index to the result of getting the navigation API entry index of destinationSHE within navigation.
        m_current_entry_index = get_the_navigation_api_entry_index(destination_she);

        // 2. Assert: navigation's current entry index is not −1.
        // NOTE: This algorithm is only called for same-document traversals.
        //       Cross-document traversals will instead call either initialize the navigation API entries for a new document
        //       or update the navigation API entries for reactivation
        VERIFY(m_current_entry_index != -1);
    }

    // 5. Otherwise, if navigationType is "push", then:
    else if (navigation_type == Bindings::NavigationType::Push) {
        // 1. Set navigation's current entry index to navigation's current entry index + 1.
        m_current_entry_index++;

        // 2. Let i be navigation's current entry index.
        auto i = m_current_entry_index;

        // 3. While i < navigation's entry list's size:
        while (i < static_cast<i64>(m_entry_list.size())) {
            // 1. Append navigation's entry list[i] to disposedNHEs.
            disposed_nhes.append(m_entry_list[i]);

            // 2. Set i to i + 1.
            ++i;
        }

        // 4. Remove all items in disposedNHEs from navigation's entry list.
        m_entry_list.remove(m_current_entry_index, m_entry_list.size() - m_current_entry_index);
    }

    // 6. Otherwise, if navigationType is "replace", then:
    else if (navigation_type == Bindings::NavigationType::Replace) {
        VERIFY(old_current_nhe != nullptr);

        // 1. Append oldCurrentNHE to disposedNHEs.
        disposed_nhes.append(*old_current_nhe);
    }

    // 7. If navigationType is "push" or "replace", then:
    if (navigation_type == Bindings::NavigationType::Push || navigation_type == Bindings::NavigationType::Replace) {
        // 1. Let newNHE be a new NavigationHistoryEntry created in the relevant realm of navigation.
        // 2. Set newNHE's session history entry to destinationSHE.
        auto new_nhe = NavigationHistoryEntry::create(realm, destination_she);

        VERIFY(m_current_entry_index != -1);

        // 3. Set navigation's entry list[navigation's current entry index] to newNHE.
        if (m_current_entry_index < static_cast<i64>(m_entry_list.size()))
            m_entry_list[m_current_entry_index] = new_nhe;
        else {
            VERIFY(m_current_entry_index == static_cast<i64>(m_entry_list.size()));
            m_entry_list.append(new_nhe);
        }
    }

    // 8. If navigation's ongoing API method tracker is non-null, then notify about the committed-to entry
    //    given navigation's ongoing API method tracker and the current entry of navigation.
    // NOTE: It is important to do this before firing the dispose or currententrychange events,
    //       since event handlers could start another navigation, or otherwise change the value of
    //       navigation's ongoing API method tracker.
    if (m_ongoing_api_method_tracker != nullptr)
        notify_about_the_committed_to_entry(*m_ongoing_api_method_tracker, *current_entry());

    // 9. Prepare to run script given navigation's relevant settings object.
    relevant_settings_object(*this).prepare_to_run_script();

    // 10. Fire an event named currententrychange at navigation using NavigationCurrentEntryChangeEvent,
    //     with its navigationType attribute initialized to navigationType and its from initialized to oldCurrentNHE.
    NavigationCurrentEntryChangeEventInit event_init = {};
    event_init.navigation_type = navigation_type;
    event_init.from = old_current_nhe;
    dispatch_event(NavigationCurrentEntryChangeEvent::construct_impl(realm, EventNames::currententrychange, event_init));

    // 11. For each disposedNHE of disposedNHEs:
    for (auto& disposed_nhe : disposed_nhes) {
        // 1. Fire an event named dispose at disposedNHE.
        disposed_nhe->dispatch_event(DOM::Event::create(realm, EventNames::dispose, {}));
    }

    // 12. Clean up after running script given navigation's relevant settings object.
    relevant_settings_object(*this).clean_up_after_running_script();
}

}
