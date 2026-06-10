/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationdestination
class NavigationDestination : public Bindings::Wrappable {
    WEB_WRAPPABLE(NavigationDestination, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(NavigationDestination);

public:
    [[nodiscard]] static GC::Ref<NavigationDestination> create();

    String url() const;
    String key() const;
    String id() const;
    i64 index() const;
    bool same_document() const;
    SerializationRecord const& state() const { return m_state; }
    WebIDL::ExceptionOr<JS::Value> get_state(JS::Realm&);

    // Non-spec'd getter, not exposed to JS
    GC::Ptr<NavigationHistoryEntry> navigation_history_entry() const { return m_entry; }

    // Setters are not available to JS, but expected in many spec algorithms
    void set_url(URL::URL const& url) { m_url = url; }
    void set_entry(GC::Ptr<NavigationHistoryEntry> entry) { m_entry = entry; }
    void set_state(SerializationRecord state) { m_state = move(state); }
    void set_is_same_document(bool b) { m_is_same_document = b; }

    virtual ~NavigationDestination() override;

    URL::URL const& raw_url() const { return m_url; }

private:
    NavigationDestination();

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationdestination-url
    URL::URL m_url;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationdestination-url
    GC::Ptr<NavigationHistoryEntry> m_entry;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationdestination-state
    SerializationRecord m_state;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationdestination-samedocument
    bool m_is_same_document { false };
};

}
