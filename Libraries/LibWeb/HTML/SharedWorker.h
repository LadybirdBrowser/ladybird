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
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
class SharedWorker final
    : public DOM::EventTarget
    , public HTML::AbstractWorker {
    WEB_PLATFORM_OBJECT(SharedWorker, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(SharedWorker);

public:
    static WebIDL::ExceptionOr<GC::Ref<SharedWorker>> construct_impl(JS::Realm&, String const& script_url, Variant<String, WorkerOptions>& options);

    virtual ~SharedWorker();

    GC::Ref<MessagePort> port() { return m_port; }

    void set_agent(WorkerAgentParent& agent) { m_agent = agent; }

private:
    SharedWorker(JS::Realm&, URL::URL script_url, WorkerOptions, MessagePort&);

    // ^AbstractWorker
    virtual DOM::EventTarget& this_event_target() override { return *this; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    URL::URL m_script_url;
    WorkerOptions m_options;
    GC::Ref<MessagePort> m_port;
    GC::Ptr<WorkerAgentParent> m_agent;
};

}
