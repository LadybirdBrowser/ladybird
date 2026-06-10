/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/AbstractWorker.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/TrustedTypes/TrustedScriptURL.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
class SharedWorker final
    : public DOM::EventTarget
    , public HTML::AbstractWorker {
    WEB_WRAPPABLE(SharedWorker, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SharedWorker);

public:
    static WebIDL::ExceptionOr<GC::Ref<SharedWorker>> create_for_constructor(JS::Realm&, TrustedTypes::TrustedScriptURLOrString const& script_url, Variant<String, WorkerOptions> options);
    static WebIDL::ExceptionOr<GC::Ref<SharedWorker>> create(WindowOrWorkerGlobalScopeMixin&, TrustedTypes::TrustedScriptURLOrString const& script_url, Variant<String, WorkerOptions> options);

    virtual ~SharedWorker();

    // https://html.spec.whatwg.org/multipage/workers.html#dom-sharedworker-port
    GC::Ref<MessagePort> port()
    {
        // The port getter steps are to return this's port.
        return m_port;
    }

    void set_agent(WorkerAgentParent& agent) { m_agent = agent; }

private:
    SharedWorker(GC::Ref<DOM::EventTarget> relevant_global_object, URL::URL script_url, WorkerOptions,
        MessagePort&);

    JS::Object& relevant_global_object() const;

    // ^AbstractWorker
    virtual DOM::EventTarget& this_event_target() override { return *this; }
    virtual void visit_edges(Cell::Visitor&) override;

    URL::URL m_script_url;
    WorkerOptions m_options;

    // Each SharedWorker has a port, a MessagePort set when the object is created.
    // https://html.spec.whatwg.org/multipage/workers.html#concept-sharedworker-port
    GC::Ref<MessagePort> m_port;
    GC::Ref<DOM::EventTarget> m_global_object;

    GC::Ptr<WorkerAgentParent> m_agent;
};

}
