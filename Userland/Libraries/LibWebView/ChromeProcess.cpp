/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibWebView/ChromeProcess.h>

namespace WebView {

static HashMap<int, RefPtr<UIProcessConnectionFromClient>> s_connections;

class UIProcessClient final
    : public IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessClient);

private:
    UIProcessClient(NonnullOwnPtr<Core::LocalSocket>);
};

ErrorOr<ChromeProcess> ChromeProcess::create()
{
    // Increase the open file limit, as the default limits on Linux cause us to run out of file descriptors with around 15 tabs open.
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 8192); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());

    return ChromeProcess {};
}

ErrorOr<ChromeProcess::ProcessDisposition> ChromeProcess::connect(Vector<ByteString> const& raw_urls, bool new_window)
{
    static constexpr auto process_name = "Ladybird"sv;

    auto [socket_path, pid_path] = TRY(Core::IPCProcess::paths_for_process(process_name));

    if (auto pid = TRY(Core::IPCProcess::get_process_pid(process_name, pid_path)); pid.has_value()) {
        TRY(connect_as_client(socket_path, raw_urls, new_window));
        return ProcessDisposition::ExitProcess;
    }

    TRY(connect_as_server(socket_path));

    m_pid_path = pid_path;
    m_pid_file = TRY(Core::File::open(pid_path, Core::File::OpenMode::Write));
    TRY(m_pid_file->write_until_depleted(ByteString::number(::getpid())));

    return ProcessDisposition::ContinueMainProcess;
}

ErrorOr<void> ChromeProcess::connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, bool new_window)
{
    auto socket = TRY(Core::LocalSocket::connect(socket_path));
    auto client = UIProcessClient::construct(move(socket));

    if (new_window) {
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewWindow>(raw_urls))
            dbgln("Failed to send CreateNewWindow message to UIProcess");
    } else {
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewTab>(raw_urls))
            dbgln("Failed to send CreateNewTab message to UIProcess");
    }

    return {};
}

ErrorOr<void> ChromeProcess::connect_as_server(ByteString const& socket_path)
{
    auto socket_fd = TRY(Core::IPCProcess::create_ipc_socket(socket_path));
    m_socket_path = socket_path;
    auto local_server = TRY(Core::LocalServer::try_create());
    TRY(local_server->take_over_fd(socket_fd));
    m_server_connection = TRY(IPC::MultiServer<UIProcessConnectionFromClient>::try_create(move(local_server)));

    m_server_connection->on_new_client = [this](auto& client) {
        client.on_new_tab = [this](auto raw_urls) {
            if (this->on_new_tab)
                this->on_new_tab(raw_urls);
        };

        client.on_new_window = [this](auto raw_urls) {
            if (this->on_new_window)
                this->on_new_window(raw_urls);
        };
    };

    return {};
}

ChromeProcess::~ChromeProcess()
{
    if (m_pid_file) {
        MUST(m_pid_file->truncate(0));
        MUST(Core::System::unlink(m_pid_path));
    }

    if (!m_socket_path.is_empty())
        MUST(Core::System::unlink(m_socket_path));
}

UIProcessClient::UIProcessClient(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(socket))
{
}

UIProcessConnectionFromClient::UIProcessConnectionFromClient(NonnullOwnPtr<Core::LocalSocket> socket, int client_id)
    : IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(socket), client_id)
{
    s_connections.set(client_id, *this);
}

void UIProcessConnectionFromClient::die()
{
    s_connections.remove(client_id());
}

void UIProcessConnectionFromClient::create_new_tab(Vector<ByteString> const& urls)
{
    if (on_new_tab)
        on_new_tab(urls);
}

void UIProcessConnectionFromClient::create_new_window(Vector<ByteString> const& urls)
{
    if (on_new_window)
        on_new_window(urls);
}

}
