/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class History final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(History, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(History);

public:
    [[nodiscard]] static GC::Ref<History> create(DOM::Document&);

    virtual ~History() override;

    WebIDL::ExceptionOr<void> push_state(JS::Realm&, JS::Value data, Utf16View unused, Optional<Utf16String> const& url = {});
    WebIDL::ExceptionOr<void> replace_state(JS::Realm&, JS::Value data, Utf16View unused, Optional<Utf16String> const& url = {});
    WebIDL::ExceptionOr<void> go(WebIDL::Long delta);
    WebIDL::ExceptionOr<void> back();
    WebIDL::ExceptionOr<void> forward();
    WebIDL::ExceptionOr<u64> length() const;
    WebIDL::ExceptionOr<Bindings::ScrollRestoration> scroll_restoration() const;
    WebIDL::ExceptionOr<void> set_scroll_restoration(Bindings::ScrollRestoration);
    WebIDL::ExceptionOr<JS::Value> state() const;
    JS::Value const& state_value() const { return m_state; }
    bool associated_document_is_fully_active() const;

    u64 m_index { 0 };
    u64 m_length { 0 };

    void set_state(JS::Value s) { m_state = s; }

private:
    explicit History(DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    WebIDL::ExceptionOr<void> delta_traverse(WebIDL::Long delta);

    WebIDL::ExceptionOr<void> shared_history_push_replace_state(JS::Realm&, JS::Value data, Optional<Utf16String> const& url, HistoryHandlingBehavior);

    GC::Ref<DOM::Document> m_document;
    JS::Value m_state { JS::js_null() };
};

bool can_have_its_url_rewritten(DOM::Document const& document, URL::URL const& target_url);

}
