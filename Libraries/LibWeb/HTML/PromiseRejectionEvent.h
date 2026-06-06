/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

class PromiseRejectionEvent final : public DOM::Event {
    WEB_WRAPPABLE(PromiseRejectionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PromiseRejectionEvent);

public:
    [[nodiscard]] static GC::Ref<PromiseRejectionEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::PromiseRejectionEventInit const&);
    [[nodiscard]] static GC::Ref<PromiseRejectionEvent> create(FlyString const& event_name, Bindings::PromiseRejectionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<PromiseRejectionEvent>> construct_impl(WindowOrWorkerGlobalScopeMixin&, FlyString const& event_name, Bindings::PromiseRejectionEventInit const&);

    virtual ~PromiseRejectionEvent() override;

    // Needs to return a pointer for the generated JS bindings to work.
    JS::Object const* promise() const { return m_promise; }
    JS::Value reason() const { return m_reason; }

private:
    PromiseRejectionEvent(FlyString const& event_name, Bindings::PromiseRejectionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<JS::Object> m_promise;
    JS::Value m_reason;
};

}
