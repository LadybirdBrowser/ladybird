/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/NavigationType.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationtransition
class NavigationTransition : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NavigationTransition, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(NavigationTransition);

public:
    [[nodiscard]] static GC::Ref<NavigationTransition> create(JS::Realm&, Bindings::NavigationType, GC::Ref<NavigationHistoryEntry>, GC::Ref<NavigationDestination>, GC::Ref<WebIDL::Promise> committed, GC::Ref<WebIDL::Promise> finished);

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationtransition-navigationtype
    Bindings::NavigationType navigation_type() const
    {
        // The navigationType getter steps are to return this's navigation type.
        return m_navigation_type;
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationtransition-from
    GC::Ref<NavigationHistoryEntry> from() const
    {
        // The from getter steps are to return this's from entry.
        return m_from_entry;
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationtransition-to
    GC::Ref<NavigationDestination> to() const
    {
        // The to getter steps are to return this's destination.
        return m_destination;
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationtransition-committed
    GC::Ref<WebIDL::Promise> committed() const
    {
        // The committed getter steps are to return this's committed promise.
        return m_committed_promise;
    }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationtransition-finished
    GC::Ref<WebIDL::Promise> finished() const
    {
        // The finished getter steps are to return this's finished promise.
        return m_finished_promise;
    }

    virtual ~NavigationTransition() override;

private:
    NavigationTransition(JS::Realm&, Bindings::NavigationType, GC::Ref<NavigationHistoryEntry>, GC::Ref<NavigationDestination>, GC::Ref<WebIDL::Promise> committed, GC::Ref<WebIDL::Promise> finished);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationtransition-navigationtype
    // Each NavigationTransition has an associated navigation type, which is a NavigationType.
    Bindings::NavigationType m_navigation_type;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationtransition-from
    // Each NavigationTransition has an associated from entry, which is a NavigationHistoryEntry.
    GC::Ref<NavigationHistoryEntry> m_from_entry;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationtransition-destination
    // Each NavigationTransition has an associated destination, which is a NavigationDestination.
    GC::Ref<NavigationDestination> m_destination;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationtransition-committedc
    // Each NavigationTransition has an associated committed promise, which is a promise.
    GC::Ref<WebIDL::Promise> m_committed_promise;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationtransition-finished
    // Each NavigationTransition has an associated finished promise, which is a promise.
    GC::Ref<WebIDL::Promise> m_finished_promise;
};

}
