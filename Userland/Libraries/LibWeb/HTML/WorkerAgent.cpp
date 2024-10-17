/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/WorkerAgent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(WorkerAgent);

WorkerAgent::WorkerAgent(URL::URL url, WorkerOptions const& options, JS::GCPtr<MessagePort> outside_port, JS::NonnullGCPtr<EnvironmentSettingsObject> outside_settings)
    : m_worker_options(options)
    , m_url(move(url))
    , m_outside_port(outside_port)
    , m_outside_settings(outside_settings)
{
}

void WorkerAgent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    m_message_port = MessagePort::create(realm);
    m_message_port->entangle_with(*m_outside_port);

    TransferDataHolder data_holder;
    MUST(m_message_port->transfer_steps(data_holder));

    // NOTE: This blocking IPC call may launch another process.
    //    If spinning the event loop for this can cause other javascript to execute, we're in trouble.
    auto worker_socket_file = Bindings::host_defined_page(realm).client().request_worker_agent();
    auto worker_socket = MUST(Core::LocalSocket::adopt_fd(worker_socket_file.take_fd()));
    MUST(worker_socket->set_blocking(true));

    m_worker_ipc = make_ref_counted<WebWorkerClient>(move(worker_socket));

    m_worker_ipc->async_start_dedicated_worker(m_url, m_worker_options.type, m_worker_options.credentials, m_worker_options.name, move(data_holder), m_outside_settings->serialize());
}

void WorkerAgent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_message_port);
    visitor.visit(m_outside_port);
    visitor.visit(m_outside_settings);
}

}
