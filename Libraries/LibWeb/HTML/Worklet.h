/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/worklets.html#worklet
class WEB_API Worklet : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(Worklet, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Worklet);

public:
    virtual ~Worklet() override;

    // https://html.spec.whatwg.org/multipage/worklets.html#dom-worklet-addmodule
    GC::Ref<WebIDL::Promise> add_module(Utf16String const& module_url, Bindings::WorkletOptions const&);

protected:
    explicit Worklet(GC::Ref<DOM::EventTarget> relevant_global);

    virtual GC::Ref<WorkletGlobalScope> create_global_scope() = 0;
    virtual Fetch::Infrastructure::Request::Destination worklet_destination() const = 0;

    GC::Ptr<WorkletGlobalScope> global_scope() const { return m_global_scope; }
    GC::Ref<DOM::EventTarget> relevant_global() const { return m_relevant_global; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<DOM::EventTarget> m_relevant_global;

    // https://html.spec.whatwg.org/multipage/worklets.html#concept-worklet-global-scopes
    // AD-HOC: The spec allows a worklet to own multiple global scopes and run each added module in
    //         all of them. Every worklet type we implement (AudioWorklet) specifies exactly one
    //         global scope per worklet, so we store one and create it lazily on first addModule().
    GC::Ptr<WorkletGlobalScope> m_global_scope;
};

}
