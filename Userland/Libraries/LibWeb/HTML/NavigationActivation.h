/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/NavigationHistoryEntry.h>
#include <LibWeb/HTML/NavigationType.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigation-interface
class NavigationActivation : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(NavigationActivation, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(NavigationActivation);

public:
    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationactivation-from
    // The from getter steps are to return this's old entry.
    JS::GCPtr<NavigationHistoryEntry> from() { return m_old_entry; }
    void set_old_entry(JS::GCPtr<NavigationHistoryEntry> entry) { m_old_entry = entry; }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationactivation-entry
    // The entry getter steps are to return this's new entry.
    JS::GCPtr<NavigationHistoryEntry> entry() { return m_new_entry; }
    void set_new_entry(JS::GCPtr<NavigationHistoryEntry> entry) { m_new_entry = entry; }

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#dom-navigationactivation-entry
    // The navigationType getter steps are to return this's navigation type.
    Bindings::NavigationType navigation_type() const { return m_navigation_type; }
    void set_navigation_type(Bindings::NavigationType type) { m_navigation_type = type; }

protected:
    explicit NavigationActivation(JS::Realm&);
    virtual ~NavigationActivation();

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

private:
    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#nav-activation-old-entry
    // old entry, null or a NavigationHistoryEntry.
    JS::GCPtr<NavigationHistoryEntry> m_old_entry;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#nav-activation-new-entry
    // new entry, null or a NavigationHistoryEntry.
    JS::GCPtr<NavigationHistoryEntry> m_new_entry;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#nav-activation-navigation-type
    // navigation type, a NavigationType.
    Bindings::NavigationType m_navigation_type {};
};

}
