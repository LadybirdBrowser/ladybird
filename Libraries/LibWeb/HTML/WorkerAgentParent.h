/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Bindings/AgentType.h>
#include <LibWeb/Bindings/Request.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>

namespace Web::HTML {

// FIXME: Figure out a better naming convention for this type of parent/child process pattern.
class WorkerAgentParent : public JS::Cell {
    GC_CELL(WorkerAgentParent, JS::Cell);
    GC_DECLARE_ALLOCATOR(WorkerAgentParent);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static WEB_API void did_finish_loading_worker_script(WorkerAgentOwnerToken);
    static WEB_API void did_fail_loading_worker_script(WorkerAgentOwnerToken);
    static WEB_API void did_report_worker_exception(WorkerAgentOwnerToken, String message, String filename, u32 lineno, u32 colno);
    static WEB_API void did_close_worker(WorkerAgentOwnerToken);

protected:
    WorkerAgentParent(URL::URL url, Bindings::WorkerOptions const& options, GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings, GC::Ref<DOM::EventTarget> worker_event_target, Bindings::AgentType);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

private:
    void release_startup_keep_alive();
    void dispatch_error_event();
    void dispatch_worker_exception(String message, String filename, u32 lineno, u32 colno);

    static WorkerAgentOwnerToken next_owner_token();

    Bindings::WorkerOptions m_worker_options;
    Bindings::AgentType m_agent_type { Bindings::AgentType::DedicatedWorker };
    URL::URL m_url;
    WorkerAgentId m_agent_id { 0 };
    WorkerAgentOwnerToken m_owner_token { 0 };

    GC::Ptr<MessagePort> m_message_port;
    GC::Ptr<MessagePort> m_outside_port;
    GC::Ref<EnvironmentSettingsObject> m_outside_settings;
    GC::Ref<DOM::EventTarget> m_worker_event_target;
};

}
