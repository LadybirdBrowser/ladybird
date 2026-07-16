/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/WorkerAgentParent.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/CompositorConnection.h>
#include <WebWorker/ConnectionFromClient.h>
#include <WebWorker/PageHost.h>
#include <WebWorker/WorkerHost.h>

namespace WebWorker {

Messages::WebWorkerServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

void ConnectionFromClient::connect_to_request_server(IPC::TransportHandle handle)
{
    if (on_request_server_connection)
        on_request_server_connection(handle);
}

void ConnectionFromClient::connect_to_image_decoder(IPC::TransportHandle handle)
{
    if (on_image_decoder_connection)
        on_image_decoder_connection(handle);
}

void ConnectionFromClient::connect_to_compositor(IPC::TransportHandle handle)
{
    auto transport = MUST(handle.create_transport());
    m_compositor_connection = adopt_ref(*new WebView::CompositorConnection(move(transport)));
    m_compositor_connection->on_compositor_lost = [this] {
        m_page_host->compositor_process_lost();
    };

#ifdef AK_OS_WINDOWS
    // Perform Windows peer PID handshake before any other IPC
    if constexpr (requires { m_compositor_connection->transport().set_peer_pid(0); }) {
        auto response = m_compositor_connection->send_sync<Messages::CompositorWebContentServer::InitTransport>(Core::System::getpid());
        m_compositor_connection->transport().set_peer_pid(response->compositor_pid());
    }
#endif
}

WebView::CompositorConnection* ConnectionFromClient::compositor_process_connection() const
{
    if (!m_compositor_connection || !m_compositor_connection->is_open())
        return nullptr;
    return m_compositor_connection.ptr();
}

void ConnectionFromClient::set_system_font_family(String family)
{
    Web::Platform::FontPlugin::the().set_system_font_family(FlyString { family });
}

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
    Core::Process::terminate_immediately(0);
}

void ConnectionFromClient::request_file(Web::FileRequest request)
{
    auto request_id = ++last_id;

    auto path = request.path();
    m_requested_files.set(request_id, move(request));
    async_did_request_file(path, request_id);
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

void ConnectionFromClient::start_worker(URL::URL url, Web::Bindings::WorkerType type, Web::Bindings::RequestCredentials credentials, Utf16String name, Web::HTML::TransferDataEncoder implicit_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings, Web::Bindings::AgentType agent_type)
{
    m_worker_host = make_ref_counted<WorkerHost>(move(url), type, move(name));

    bool const is_shared = agent_type == Web::Bindings::AgentType::SharedWorker;
    VERIFY(is_shared || agent_type == Web::Bindings::AgentType::DedicatedWorker);

    // FIXME: Add an assertion that the agent_type passed here is the same that was passed at process creation to initialize_main_thread_vm()

    m_worker_host->run(page(), move(implicit_port), outside_settings, credentials, is_shared);
}

void ConnectionFromClient::connect_shared_worker(Web::HTML::TransferDataEncoder message_port, Web::HTML::SerializedEnvironmentSettingsObject outside_settings)
{
    if (!m_worker_host)
        return;
    m_worker_host->connect_shared_worker(move(message_port), move(outside_settings));
}

void ConnectionFromClient::handle_file_return(i32 error, Optional<IPC::File> file, i32 request_id)
{
    auto file_request = m_requested_files.take(request_id);

    VERIFY(file_request.has_value());
    VERIFY(file_request.value().on_file_request_finish);

    file_request.value().on_file_request_finish(error != 0 ? Error::from_errno(error) : ErrorOr<i32> { file->take_fd() });
}

void ConnectionFromClient::did_worker_agent_finish_loading_script(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_finish_loading_worker_script(owner_token);
}

void ConnectionFromClient::did_worker_agent_fail_loading_script(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_fail_loading_worker_script(owner_token);
}

void ConnectionFromClient::did_worker_agent_report_exception(Web::HTML::WorkerAgentOwnerToken owner_token, Utf16String message, Utf16String filename, u32 lineno, u32 colno)
{
    Web::HTML::WorkerAgentParent::did_report_worker_exception(owner_token, move(message), move(filename), lineno, colno);
}

void ConnectionFromClient::did_worker_agent_close(Web::HTML::WorkerAgentOwnerToken owner_token)
{
    Web::HTML::WorkerAgentParent::did_close_worker(owner_token);
}

void ConnectionFromClient::broadcast_channel_message(Web::HTML::BroadcastChannelMessage message)
{
    Web::HTML::BroadcastChannel::deliver_message_locally(message);
}

}
