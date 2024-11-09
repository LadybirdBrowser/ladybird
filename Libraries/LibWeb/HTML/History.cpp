/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HistoryPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/History.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(History);

JS::NonnullGCPtr<History> History::create(JS::Realm& realm, DOM::Document& document)
{
    return realm.heap().allocate<History>(realm, realm, document);
}

History::History(JS::Realm& realm, DOM::Document& document)
    : PlatformObject(realm)
    , m_associated_document(document)
{
}

History::~History() = default;

void History::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(History);
}

void History::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_document);
    visitor.visit(m_state);
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-pushstate
// The pushState(data, unused, url) method steps are to run the shared history push/replace state steps given this, data, url, and "push".
WebIDL::ExceptionOr<void> History::push_state(JS::Value data, String const&, Optional<String> const& url)
{
    return shared_history_push_replace_state(data, url, HistoryHandlingBehavior::Push);
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-replacestate
// The replaceState(data, unused, url) method steps are to run the shared history push/replace state steps given this, data, url, and "replace".
WebIDL::ExceptionOr<void> History::replace_state(JS::Value data, String const&, Optional<String> const& url)
{
    return shared_history_push_replace_state(data, url, HistoryHandlingBehavior::Replace);
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-length
WebIDL::ExceptionOr<u64> History::length() const
{
    // 1. If this's relevant global object's associated Document is not fully active, then throw a "SecurityError" DOMException.
    if (!m_associated_document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot perform length on a document that isn't fully active."_string);

    // 2. Return this's length.
    return m_length;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-history-state
WebIDL::ExceptionOr<JS::Value> History::state() const
{
    // 1. If this's relevant global object's associated Document is not fully active, then throw a "SecurityError" DOMException.
    if (!m_associated_document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot perform state on a document that isn't fully active."_string);

    // 2. Return this's state.
    return m_state;
}

JS::Value History::unsafe_state() const
{
    return m_state;
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-go
WebIDL::ExceptionOr<void> History::go(WebIDL::Long delta = 0)
{
    // 1. Let document be this's associated Document.

    // 2. If document is not fully active, then throw a "SecurityError" DOMException.
    if (!m_associated_document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot perform go on a document that isn't fully active."_string);

    VERIFY(m_associated_document->navigable());

    // 3. If delta is 0, then reload document's node navigable.
    if (delta == 0)
        m_associated_document->navigable()->reload();

    // 4. Traverse the history by a delta given document's node navigable's traversable navigable, delta, and with sourceDocument set to document.
    auto traversable = m_associated_document->navigable()->traversable_navigable();
    traversable->traverse_the_history_by_delta(delta);

    return {};
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-back
WebIDL::ExceptionOr<void> History::back()
{
    // 1. Let document be this's associated Document.
    // 2. If document is not fully active, then throw a "SecurityError" DOMException.
    // NOTE: We already did this check in `go` method, so skip the fully active check here.

    // 3. Traverse the history by a delta with −1 and document's browsing context.
    return go(-1);
}

// https://html.spec.whatwg.org/multipage/history.html#dom-history-forward
WebIDL::ExceptionOr<void> History::forward()
{
    // 1. Let document be this's associated Document.
    // 2. If document is not fully active, then throw a "SecurityError" DOMException.
    // NOTE: We already did this check in `go` method, so skip the fully active check here.

    // 3. Traverse the history by a delta with +1 and document's browsing context.
    return go(1);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#can-have-its-url-rewritten
bool can_have_its_url_rewritten(DOM::Document const& document, URL::URL const& target_url)
{
    // 1. Let documentURL be document's URL.
    auto document_url = document.url();

    // 2. If targetURL and documentURL differ in their scheme, username, password, host, or port components,
    //    then return false.
    if (target_url.scheme() != document_url.scheme()
        || target_url.username() != document_url.username()
        || target_url.password() != document_url.password()
        || target_url.host() != document_url.host()
        || target_url.port() != document_url.port())
        return false;

    // 3. If targetURL's scheme is an HTTP(S) scheme, then return true.
    //    (Differences in path, query, and fragment are allowed for http: and https: URLs.)
    if (target_url.scheme() == "http"sv || target_url.scheme() == "https"sv)
        return true;

    // 4. If targetURL's scheme is "file", then:
    //    (Differences in query and fragment are allowed for file: URLs.)
    if (target_url.scheme() == "file"sv) {
        // 1. If targetURL and documentURL differ in their path component, then return false.
        if (target_url.paths() != document_url.paths())
            return false;

        // 2. Return true.
        return true;
    }

    // 5. If targetURL and documentURL differ in their path component or query components, then return false.
    //    (Only differences in fragment are allowed for other types of URLs.)
    if (target_url.paths() != document_url.paths() || target_url.query() != document_url.query())
        return false;

    // 6. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/history.html#shared-history-push/replace-state-steps
WebIDL::ExceptionOr<void> History::shared_history_push_replace_state(JS::Value data, Optional<String> const& url, HistoryHandlingBehavior history_handling)
{
    auto& vm = this->vm();

    // 1. Let document be history's associated Document.
    auto& document = m_associated_document;

    // 2. If document is not fully active, then throw a "SecurityError" DOMException.
    if (!document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot perform pushState or replaceState on a document that isn't fully active."_string);

    // 3. Optionally, return. (For example, the user agent might disallow calls to these methods that are invoked on a timer,
    //    or from event listeners that are not triggered in response to a clear user action, or that are invoked in rapid succession.)

    // 4. Let serializedData be StructuredSerializeForStorage(data). Rethrow any exceptions.
    //    FIXME: Actually rethrow exceptions here once we start using the serialized data.
    //           Throwing here on data types we don't yet serialize will regress sites that use push/replaceState.
    auto serialized_data_or_error = structured_serialize_for_storage(vm, data);
    auto serialized_data = serialized_data_or_error.is_error() ? MUST(structured_serialize_for_storage(vm, JS::js_null())) : serialized_data_or_error.release_value();

    // 5. Let newURL be document's URL.
    auto new_url = document->url();

    // 6. If url is not null or the empty string, then:
    if (url.has_value() && !url->is_empty()) {

        // 1. Parse url, relative to the relevant settings object of history.
        auto parsed_url = relevant_settings_object(*this).parse_url(url->to_byte_string());

        // 2. If that fails, then throw a "SecurityError" DOMException.
        if (!parsed_url.is_valid())
            return WebIDL::SecurityError::create(realm(), "Cannot pushState or replaceState to incompatible URL"_string);

        // 3. Set newURL to the resulting URL record.
        new_url = parsed_url;

        // 4. If document cannot have its URL rewritten to newURL, then throw a "SecurityError" DOMException.
        if (!can_have_its_url_rewritten(document, new_url))
            return WebIDL::SecurityError::create(realm(), "Cannot pushState or replaceState to incompatible URL"_string);
    }

    // 7. Let navigation be history's relevant global object's navigation API.
    auto navigation = verify_cast<Window>(relevant_global_object(*this)).navigation();

    // 8. Let continue be the result of firing a push/replace/reload navigate event at navigation
    //    with navigationType set to historyHandling, isSameDocument set to true, destinationURL set to newURL,
    //    and classicHistoryAPIState set to serializedData.
    auto navigation_type = history_handling == HistoryHandlingBehavior::Push ? Bindings::NavigationType::Push : Bindings::NavigationType::Replace;
    auto continue_ = navigation->fire_a_push_replace_reload_navigate_event(navigation_type, new_url, true, UserNavigationInvolvement::None, {}, {}, serialized_data);
    // 9. If continue is false, then return.
    if (!continue_)
        return {};

    // 10. Run the URL and history update steps given document and newURL, with serializedData set to
    //     serializedData and historyHandling set to historyHandling.
    perform_url_and_history_update_steps(document, new_url, serialized_data, history_handling);

    return {};
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-history-scroll-restoration
WebIDL::ExceptionOr<Bindings::ScrollRestoration> History::scroll_restoration() const
{
    // 1. If this's relevant global object's associated Document is not fully active, then throw a "SecurityError" DOMException.
    if (!m_associated_document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot obtain scroll restoration mode for a document that isn't fully active."_string);

    // 2. Return this's node navigable's active session history entry's scroll restoration mode.
    auto scroll_restoration_mode = m_associated_document->navigable()->active_session_history_entry()->scroll_restoration_mode();
    switch (scroll_restoration_mode) {
    case ScrollRestorationMode::Auto:
        return Bindings::ScrollRestoration::Auto;
    case ScrollRestorationMode::Manual:
        return Bindings::ScrollRestoration::Manual;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-history-scroll-restoration
WebIDL::ExceptionOr<void> History::set_scroll_restoration(Bindings::ScrollRestoration scroll_restoration)
{
    // 1. If this's relevant global object's associated Document is not fully active, then throw a "SecurityError" DOMException.
    if (!m_associated_document->is_fully_active())
        return WebIDL::SecurityError::create(realm(), "Cannot set scroll restoration mode for a document that isn't fully active."_string);

    // 2. Set this's node navigable's active session history entry's scroll restoration mode to the given value.
    auto active_session_history_entry = m_associated_document->navigable()->active_session_history_entry();
    switch (scroll_restoration) {
    case Bindings::ScrollRestoration::Auto:
        active_session_history_entry->set_scroll_restoration_mode(ScrollRestorationMode::Auto);
        break;
    case Bindings::ScrollRestoration::Manual:
        active_session_history_entry->set_scroll_restoration_mode(ScrollRestorationMode::Manual);
        break;
    }

    return {};
}

}
