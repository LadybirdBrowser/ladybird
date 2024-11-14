/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/AbortSignal.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#abortcontroller
class AbortController final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AbortController, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(AbortController);

public:
    static WebIDL::ExceptionOr<GC::Ref<AbortController>> construct_impl(JS::Realm&);

    virtual ~AbortController() override;

    // https://dom.spec.whatwg.org/#dom-abortcontroller-signal
    GC::Ref<AbortSignal> signal() const { return *m_signal; }

    void abort(JS::Value reason);

private:
    AbortController(JS::Realm&, GC::Ref<AbortSignal>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://dom.spec.whatwg.org/#abortcontroller-signal
    GC::Ref<AbortSignal> m_signal;
};

}
