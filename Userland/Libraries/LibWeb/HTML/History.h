/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/HistoryPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/HistoryHandlingBehavior.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class History final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(History, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(History);

public:
    [[nodiscard]] static JS::NonnullGCPtr<History> create(JS::Realm&, DOM::Document&);

    virtual ~History() override;

    WebIDL::ExceptionOr<void> push_state(JS::Value data, String const& unused, Optional<String> const& url = {});
    WebIDL::ExceptionOr<void> replace_state(JS::Value data, String const& unused, Optional<String> const& url = {});
    WebIDL::ExceptionOr<void> go(WebIDL::Long delta);
    WebIDL::ExceptionOr<void> back();
    WebIDL::ExceptionOr<void> forward();
    WebIDL::ExceptionOr<u64> length() const;
    WebIDL::ExceptionOr<Bindings::ScrollRestoration> scroll_restoration() const;
    WebIDL::ExceptionOr<void> set_scroll_restoration(Bindings::ScrollRestoration);
    WebIDL::ExceptionOr<JS::Value> state() const;

    u64 m_index { 0 };
    u64 m_length { 0 };

    JS::Value unsafe_state() const;
    void set_state(JS::Value s) { m_state = s; }

private:
    History(JS::Realm&, DOM::Document&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> shared_history_push_replace_state(JS::Value data, Optional<String> const& url, HistoryHandlingBehavior);

    JS::NonnullGCPtr<DOM::Document> m_associated_document;
    JS::Value m_state { JS::js_null() };
};

bool can_have_its_url_rewritten(DOM::Document const& document, URL::URL const& target_url);

}
