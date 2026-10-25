/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

class NavigateEvent;

// https://html.spec.whatwg.org/multipage/nav-history-apis.html#navigationprecommitcontroller
class NavigationPrecommitController : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(NavigationPrecommitController, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(NavigationPrecommitController);

public:
    [[nodiscard]] static GC::Ref<NavigationPrecommitController> create(GC::Ref<NavigateEvent>);

    virtual ~NavigationPrecommitController() override;

    WebIDL::ExceptionOr<void> redirect(Utf16String const& url, Bindings::NavigationNavigateOptions const&);
    WebIDL::ExceptionOr<void> add_handler(GC::Ref<WebIDL::CallbackType> handler);

private:
    explicit NavigationPrecommitController(GC::Ref<NavigateEvent>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/nav-history-apis.html#concept-navigationprecommitcontroller-event
    GC::Ref<NavigateEvent> m_event;
};

}
