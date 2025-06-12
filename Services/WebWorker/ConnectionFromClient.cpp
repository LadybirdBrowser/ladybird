/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <WebWorker/ConnectionFromClient.h>
#include <WebWorker/PageHost.h>
#include <WebWorker/WorkerHost.h>

namespace WebWorker {

void ConnectionFromClient::close_worker()
{
    async_did_close_worker();

    // FIXME: Invoke a worker shutdown operation that implements the spec
    m_worker_host = nullptr;

    die();
}

void ConnectionFromClient::die()
{
    // FIXME: When handling multiple workers in the same process,
    //     this logic needs to be smarter (only when all workers are dead, etc).
    Core::EventLoop::current().quit(0);
}

void ConnectionFromClient::request_file(Web::FileRequest request)
{
    // FIXME: Route this to FSAS or browser process as appropriate instead of allowing
    //        the WebWorker process filesystem access
    auto path = request.path();
    auto request_id = ++last_id;

    m_requested_files.set(request_id, move(request));

    auto file = Core::File::open(path, Core::File::OpenMode::Read);

    if (file.is_error())
        handle_file_return(file.error().code(), {}, request_id);
    else
        handle_file_return(0, IPC::File::adopt_file(file.release_value()), request_id);
}

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<WebWorkerClientEndpoint, WebWorkerServerEndpoint>(*this, move(transport), 1)
    , m_page_host(PageHost::create(Web::Bindings::main_thread_vm(), *this))
{
}

ConnectionFromClient::~ConnectionFromClient() = default;

Web::Page& ConnectionFromClient::page()
{
    return m_page_host->page();
}

Web::Page const& ConnectionFromClient::page() const
{
    return m_page_host->page();
}

void ConnectionFromClient::start_worker(URL::URL url, Web::Bindings::WorkerType type, Web::Bindings::RequestCredentials credentials, String name, Web::HTML::TransferDataHolder implicit_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings, Web::Bindings::AgentType agent_type)
{
    m_worker_host = make_ref_counted<WorkerHost>(move(url), type, move(name));

    bool const is_shared = agent_type == Web::Bindings::AgentType::SharedWorker;
    VERIFY(is_shared || agent_type == Web::Bindings::AgentType::DedicatedWorker);

    // FIXME: Add an assertion that the agent_type passed here is the same that was passed at process creation to initialize_main_thread_vm()

    m_worker_host->run(page(), move(implicit_port), outside_settings, credentials, is_shared);
}

void ConnectionFromClient::handle_file_return(i32 error, Optional<IPC::File> file, i32 request_id)
{
    auto file_request = m_requested_files.take(request_id);

    VERIFY(file_request.has_value());
    VERIFY(file_request.value().on_file_request_finish);

    file_request.value().on_file_request_finish(error != 0 ? Error::from_errno(error) : ErrorOr<i32> { file->take_fd() });
}

}
