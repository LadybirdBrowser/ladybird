/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NavigationDestinationPrototype.h>
#include <LibWeb/HTML/NavigationDestination.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(NavigationDestination);

JS::NonnullGCPtr<NavigationDestination> NavigationDestination::create(JS::Realm& realm)
{
    return realm.heap().allocate<NavigationDestination>(realm, realm);
}

NavigationDestination::NavigationDestination(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

NavigationDestination::~NavigationDestination() = default;

void NavigationDestination::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NavigationDestination);
}

void NavigationDestination::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_entry);
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-url
WebIDL::ExceptionOr<String> NavigationDestination::url() const
{
    // The url getter steps are to return this's URL, serialized.
    return TRY_OR_THROW_OOM(vm(), String::from_byte_string(m_url.serialize()));
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-key
String NavigationDestination::key() const
{
    // The key getter steps are:

    // 1. If this's entry is null, then return the empty string.
    // 2. Return this's entry's key.
    return (m_entry == nullptr) ? String {} : m_entry->key();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-id
String NavigationDestination::id() const
{
    // The id getter steps are:

    // 1. If this's entry is null, then return the empty string.
    // 2. Return this's entry's ID.
    return (m_entry == nullptr) ? String {} : m_entry->id();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-index
i64 NavigationDestination::index() const
{
    // The index getter steps are:

    // 1. If this's entry is null, then return -1.
    // 2. Return this's entry's index.
    return (m_entry == nullptr) ? -1 : m_entry->index();
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-samedocument
bool NavigationDestination::same_document() const
{
    // The sameDocument getter steps are to return this's is same document.
    return m_is_same_document;
}

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationdestination-getstate
WebIDL::ExceptionOr<JS::Value> NavigationDestination::get_state()
{
    // The getState() method steps are to return StructuredDeserialize(this's state).
    return structured_deserialize(vm(), m_state, realm(), {});
}

}
