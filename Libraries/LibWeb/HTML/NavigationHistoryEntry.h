/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationhistoryentry
class NavigationHistoryEntry : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(NavigationHistoryEntry, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(NavigationHistoryEntry);

public:
    [[nodiscard]] static JS::NonnullGCPtr<NavigationHistoryEntry> create(JS::Realm&, JS::NonnullGCPtr<SessionHistoryEntry>);

    WebIDL::ExceptionOr<Optional<String>> url() const;
    String key() const;
    String id() const;
    i64 index() const;
    bool same_document() const;
    WebIDL::ExceptionOr<JS::Value> get_state();

    void set_ondispose(WebIDL::CallbackType*);
    WebIDL::CallbackType* ondispose();

    // Non-spec'd getter, not exposed to JS
    SessionHistoryEntry const& session_history_entry() const { return *m_session_history_entry; }
    SessionHistoryEntry& session_history_entry() { return *m_session_history_entry; }

    virtual ~NavigationHistoryEntry() override;

private:
    NavigationHistoryEntry(JS::Realm&, JS::NonnullGCPtr<SessionHistoryEntry>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::NonnullGCPtr<SessionHistoryEntry> m_session_history_entry;
};
}
