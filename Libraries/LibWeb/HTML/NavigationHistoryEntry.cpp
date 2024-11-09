/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigationHistoryEntryPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/Navigation.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationHistoryEntry);

JS::NonnullGCPtr<NavigationHistoryEntry> NavigationHistoryEntry::create(JS::Realm& realm, JS::NonnullGCPtr<SessionHistoryEntry> she)
{
    return realm.heap().allocate<NavigationHistoryEntry>(realm, realm, she);
}

NavigationHistoryEntry::NavigationHistoryEntry(JS::Realm& realm, JS::NonnullGCPtr<SessionHistoryEntry> she)
    : DOM::EventTarget(realm)
    , m_session_history_entry(she)
{
}

NavigationHistoryEntry::~NavigationHistoryEntry() = default;

void NavigationHistoryEntry::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NavigationHistoryEntry);
}

void NavigationHistoryEntry::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_session_history_entry);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationhistoryentry-url
WebIDL::ExceptionOr<Optional<String>> NavigationHistoryEntry::url() const
{
    // The url getter steps are:
    // 1. Let document be this's relevant global object's associated Document.
    auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 2. If document is not fully active, then return the empty string.
    if (!document.is_fully_active())
        return String {};

    // 3. Let she be this's session history entry.
    auto const& she = this->m_session_history_entry;

    // 4. If she's document does not equal document, and she's document state's request referrer policy
    //    is "no-referrer" or "origin", then return null.
    if ((she->document() != &document)
        && (she->document_state()->request_referrer_policy() == ReferrerPolicy::ReferrerPolicy::NoReferrer
            || she->document_state()->request_referrer_policy() == ReferrerPolicy::ReferrerPolicy::Origin))
        return OptionalNone {};

    // 5. Return she's URL, serialized.
    return TRY_OR_THROW_OOM(vm(), String::from_byte_string(she->url().serialize()));
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationhistoryentry-key
String NavigationHistoryEntry::key() const
{
    // The key of a NavigationHistoryEntry nhe is given by the return value of the following algorithm:
    // 1. If nhe's relevant global object's associated Document is not fully active, then return the empty string.
    auto& associated_document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return {};

    // 2. Return nhe's session history entry's navigation API key.
    return m_session_history_entry->navigation_api_key();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationhistoryentry-id
String NavigationHistoryEntry::id() const
{
    // The ID of a NavigationHistoryEntry nhe is given by the return value of the following algorithm:
    // 1. If nhe's relevant global object's associated Document is not fully active, then return the empty string.
    auto& associated_document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return {};

    // 2. Return nhe's session history entry's navigation API ID.
    return m_session_history_entry->navigation_api_id();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationhistoryentry-index
i64 NavigationHistoryEntry::index() const
{
    // The index of a NavigationHistoryEntry nhe is given by the return value of the following algorithm:
    // 1. If nhe's relevant global object's associated Document is not fully active, then return −1.
    auto& this_relevant_global_object = verify_cast<HTML::Window>(relevant_global_object(*this));
    if (!this_relevant_global_object.associated_document().is_fully_active())
        return -1;

    // 2. Return the result of getting the navigation API entry index of this's session history entry
    // within this's relevant global object's navigation API.
    return this_relevant_global_object.navigation()->get_the_navigation_api_entry_index(*m_session_history_entry);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationhistoryentry-samedocument
bool NavigationHistoryEntry::same_document() const
{
    // The sameDocument getter steps are:
    // 1. Let document be this's relevant global object's associated Document.
    auto& document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 2. If document is not fully active, then return false.
    if (!document.is_fully_active())
        return false;

    // 3. Return true if this's session history entry's document equals document, and false otherwise.
    return m_session_history_entry->document() == &document;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationhistoryentry-getstate
WebIDL::ExceptionOr<JS::Value> NavigationHistoryEntry::get_state()
{
    // The getState() method steps are:
    // 1. If this's relevant global object's associated Document is not fully active, then return undefined.
    auto& associated_document = verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active())
        return JS::js_undefined();

    // 2. Return StructuredDeserialize(this's session history entry's navigation API state). Rethrow any exceptions.
    //    NOTE: This can in theory throw an exception, if attempting to deserialize a large ArrayBuffer
    //          when not enough memory is available.
    return structured_deserialize(vm(), m_session_history_entry->navigation_api_state(), realm(), {});
}

void NavigationHistoryEntry::set_ondispose(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::dispose, event_handler);
}

WebIDL::CallbackType* NavigationHistoryEntry::ondispose()
{
    return event_handler_attribute(HTML::EventNames::dispose);
}

}
