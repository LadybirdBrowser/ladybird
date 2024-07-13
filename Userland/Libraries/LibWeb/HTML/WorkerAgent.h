/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

struct WorkerOptions {
    String type { "classic"_string };
    String credentials { "same-origin"_string };
    String name { String {} };
};

class WorkerAgent : public JS::Cell {
    JS_CELL(WorkerAgent, JS::Cell);
    JS_DECLARE_ALLOCATOR(WorkerAgent);

    WorkerAgent(URL::URL url, WorkerOptions const& options, JS::GCPtr<MessagePort> outside_port, JS::NonnullGCPtr<EnvironmentSettingsObject> outside_settings);

    void terminate();

private:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    WorkerOptions m_worker_options;
    URL::URL m_url;

    JS::GCPtr<MessagePort> m_message_port;
    JS::GCPtr<MessagePort> m_outside_port;
    JS::NonnullGCPtr<EnvironmentSettingsObject> m_outside_settings;

    RefPtr<Web::HTML::WebWorkerClient> m_worker_ipc;
};

}
