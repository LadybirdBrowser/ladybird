/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/History.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class History final : public Bindings::Wrappable {
    WEB_WRAPPABLE(History, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(History);

public:
    [[nodiscard]] static GC::Ref<History> create(DOM::Document&);

    virtual ~History() override;

    WebIDL::ExceptionOr<void> push_state(JS::Realm&, JS::Value data, String const& unused, Optional<String> const& url = {});
    WebIDL::ExceptionOr<void> replace_state(JS::Realm&, JS::Value data, String const& unused, Optional<String> const& url = {});
    WebIDL::ExceptionOr<void> go(JS::Realm&, WebIDL::Long delta);
    WebIDL::ExceptionOr<void> back(JS::Realm&);
    WebIDL::ExceptionOr<void> forward(JS::Realm&);
    WebIDL::ExceptionOr<u64> length(JS::Realm&) const;
    WebIDL::ExceptionOr<Bindings::ScrollRestoration> scroll_restoration(JS::Realm&) const;
    WebIDL::ExceptionOr<void> set_scroll_restoration(JS::Realm&, Bindings::ScrollRestoration);
    WebIDL::ExceptionOr<JS::Value> state(JS::Realm&) const;

    u64 m_index { 0 };
    u64 m_length { 0 };

    JS::Value unsafe_state() const;
    void set_state(JS::Value s) { m_state = s; }

private:
    explicit History(DOM::Document&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    WebIDL::ExceptionOr<void> delta_traverse(JS::Realm&, WebIDL::Long delta);

    WebIDL::ExceptionOr<void> shared_history_push_replace_state(JS::Realm&, JS::Value data, Optional<String> const& url, HistoryHandlingBehavior);

    GC::Ref<DOM::Document> m_document;
    JS::Value m_state { JS::js_null() };
};

bool can_have_its_url_rewritten(DOM::Document const& document, URL::URL const& target_url);

}
