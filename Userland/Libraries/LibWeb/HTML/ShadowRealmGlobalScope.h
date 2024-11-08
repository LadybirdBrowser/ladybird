/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

// https://whatpr.org/html/9893/webappapis.html#shadowrealmglobalscope
class ShadowRealmGlobalScope : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(ShadowRealmGlobalScope, DOM::EventTarget);
    JS_DECLARE_ALLOCATOR(ShadowRealmGlobalScope);

public:
    virtual ~ShadowRealmGlobalScope() override;

    static JS::NonnullGCPtr<ShadowRealmGlobalScope> create(JS::Realm&);

    // https://whatpr.org/html/9893/webappapis.html#dom-shadowrealmglobalscope-self
    JS::NonnullGCPtr<ShadowRealmGlobalScope> self()
    {
        // The self getter steps are to return this.
        return *this;
    }

    void initialize_web_interfaces();

protected:
    explicit ShadowRealmGlobalScope(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
