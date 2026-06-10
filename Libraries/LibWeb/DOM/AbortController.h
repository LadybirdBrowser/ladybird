/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/AbortSignal.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#abortcontroller
class AbortController final : public Bindings::Wrappable {
    WEB_WRAPPABLE(AbortController, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(AbortController);

public:
    static GC::Ref<AbortController> create();

    virtual ~AbortController() override;

    // https://dom.spec.whatwg.org/#dom-abortcontroller-signal
    GC::Ref<AbortSignal> signal() const { return *m_signal; }

    void abort(JS::Realm&, Optional<JS::Value> reason);
    void abort(JS::Realm&, GC::Ref<WebIDL::DOMException> reason);

private:
    AbortController(GC::Ref<AbortSignal>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://dom.spec.whatwg.org/#abortcontroller-signal
    GC::Ref<AbortSignal> m_signal;
};

}
