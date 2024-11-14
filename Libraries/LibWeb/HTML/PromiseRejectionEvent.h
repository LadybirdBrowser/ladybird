/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

struct PromiseRejectionEventInit : public DOM::EventInit {
    GC::Root<JS::Object> promise;
    JS::Value reason;
};

class PromiseRejectionEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(PromiseRejectionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PromiseRejectionEvent);

public:
    [[nodiscard]] static GC::Ref<PromiseRejectionEvent> create(JS::Realm&, FlyString const& event_name, PromiseRejectionEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<PromiseRejectionEvent>> construct_impl(JS::Realm&, FlyString const& event_name, PromiseRejectionEventInit const&);

    virtual ~PromiseRejectionEvent() override;

    // Needs to return a pointer for the generated JS bindings to work.
    JS::Object const* promise() const { return m_promise; }
    JS::Value reason() const { return m_reason; }

private:
    PromiseRejectionEvent(JS::Realm&, FlyString const& event_name, PromiseRejectionEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<JS::Object> m_promise;
    JS::Value m_reason;
};

}
