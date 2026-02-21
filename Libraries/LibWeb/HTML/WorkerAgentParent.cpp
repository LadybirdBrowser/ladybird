/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Worker/WebWorkerClient.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerAgentParent);

WorkerAgentParent::WorkerAgentParent(URL::URL url, WorkerOptions const& options, GC::Ptr<MessagePort> outside_port, GC::Ref<EnvironmentSettingsObject> outside_settings, Bindings::AgentType agent_type)
    : m_worker_options(options)
    , m_agent_type(agent_type)
    , m_url(move(url))
    , m_outside_port(outside_port)
    , m_outside_settings(outside_settings)
{
}

void WorkerAgentParent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    m_message_port = MessagePort::create(realm);
    m_message_port->entangle_with(*m_outside_port);

    TransferDataEncoder data_holder;
    MUST(m_message_port->transfer_steps(data_holder));

    // FIXME: Specification says this supposed to happen in step 11 of onComplete handler defined in https://html.spec.whatwg.org/multipage/workers.html#run-a-worker
    //        but that would require introducing a new IPC message type to communicate this from WebWorker to WebContent process,
    //        so let's do it here for now.
    m_outside_port->start();

    // NOTE: This blocking IPC call may launch another process.
    //    If spinning the event loop for this can cause other javascript to execute, we're in trouble.
    auto worker_socket_file = Bindings::principal_host_defined_page(realm).client().request_worker_agent(m_agent_type);

    auto worker_socket = MUST(Core::LocalSocket::adopt_fd(worker_socket_file.take_fd()));
    MUST(worker_socket->set_blocking(true));

    // TODO: Mach IPC
    auto transport = make<IPC::Transport>(move(worker_socket));

    m_worker_ipc = make_ref_counted<WebWorkerClient>(move(transport));
    setup_worker_ipc_callbacks(realm);

    auto serialized_outside_settings = m_outside_settings->serialize();

    m_worker_ipc->async_start_worker(m_url, m_worker_options.type, m_worker_options.credentials, m_worker_options.name, move(data_holder), serialized_outside_settings, m_agent_type);
}

void WorkerAgentParent::setup_worker_ipc_callbacks(JS::Realm& realm)
{
    // NOTE: As long as WorkerAgentParent is alive, realm and m_worker_ipc will be alive.
    m_worker_ipc->on_request_cookie = [realm = GC::RawRef { realm }](URL::URL const& url, HTTP::Cookie::Source source) {
        auto& client = Bindings::principal_host_defined_page(realm).client();
        return client.page_did_request_cookie(url, source);
    };
    m_worker_ipc->on_request_worker_agent = [realm = GC::RawRef { realm }](Web::Bindings::AgentType worker_type) {
        auto& client = Bindings::principal_host_defined_page(realm).client();
        return client.request_worker_agent(worker_type);
    };
}

void WorkerAgentParent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_message_port);
    visitor.visit(m_outside_port);
    visitor.visit(m_outside_settings);
}

}
