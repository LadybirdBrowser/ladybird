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
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/URL.h>

namespace WebView {

static HashMap<int, RefPtr<UIProcessConnectionFromClient>> s_connections;

class UIProcessClient final
    : public IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessClient);

private:
    explicit UIProcessClient(NonnullOwnPtr<IPC::Transport>);
};

ErrorOr<BrowserProcess::ProcessDisposition> BrowserProcess::connect(Vector<ByteString> const& raw_urls, NewWindow new_window)
{
    static constexpr auto process_name = "Ladybird"sv;

    auto [socket_path, pid_path] = TRY(Process::paths_for_process(process_name));

    if (auto pid = TRY(Process::get_process_pid(process_name, pid_path)); pid.has_value()) {
        TRY(connect_as_client(socket_path, raw_urls, new_window));
        return ProcessDisposition::ExitProcess;
    }

    TRY(connect_as_server(socket_path));

    m_pid_path = pid_path;
    m_pid_file = TRY(Core::File::open(pid_path, Core::File::OpenMode::Write));
    TRY(m_pid_file->write_until_depleted(ByteString::number(Core::System::getpid())));

    return ProcessDisposition::ContinueMainProcess;
}

ErrorOr<void> BrowserProcess::connect_as_client([[maybe_unused]] ByteString const& socket_path, [[maybe_unused]] Vector<ByteString> const& raw_urls, [[maybe_unused]] NewWindow new_window)
{
#if !defined(AK_OS_WINDOWS)
    // TODO: Mach IPC
    auto socket = TRY(Core::LocalSocket::connect(socket_path));
    auto client = UIProcessClient::construct(make<IPC::Transport>(move(socket)));

    if (new_window == NewWindow::Yes) {
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewWindow>(raw_urls))
            dbgln("Failed to send CreateNewWindow message to UIProcess");
    } else {
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewTab>(raw_urls))
            dbgln("Failed to send CreateNewTab message to UIProcess");
    }

    return {};
#else
    return Error::from_string_literal("BrowserProcess::connect_as_client() is not implemented on Windows");
#endif
}

ErrorOr<void> BrowserProcess::connect_as_server([[maybe_unused]] ByteString const& socket_path)
{
#if !defined(AK_OS_WINDOWS)
    // TODO: Mach IPC
    auto socket_fd = TRY(Process::create_ipc_socket(socket_path));
    m_socket_path = socket_path;
    auto local_server = Core::LocalServer::construct();
    TRY(local_server->take_over_fd(socket_fd));

    m_server_connection = TRY(IPC::MultiServer<UIProcessConnectionFromClient>::try_create(move(local_server)));

    m_server_connection->on_new_client = [this](auto& client) {
        client.on_new_tab = [this](auto const& raw_urls) {
            if (this->on_new_tab)
                this->on_new_tab(raw_urls);
        };

        client.on_new_window = [this](auto const& raw_urls) {
            if (this->on_new_window)
                this->on_new_window(raw_urls);
        };
    };

    return {};
#else
    return Error::from_string_literal("BrowserProcess::connect_as_server() is not implemented on Windows");
#endif
}

BrowserProcess::~BrowserProcess()
{
    if (m_pid_file) {
        MUST(m_pid_file->truncate(0));
        MUST(Core::System::unlink(m_pid_path));
    }

    if (!m_socket_path.is_empty())
        MUST(Core::System::unlink(m_socket_path));
}

UIProcessClient::UIProcessClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(transport))
{
}

UIProcessConnectionFromClient::UIProcessConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, int client_id)
    : IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(transport), client_id)
{
    s_connections.set(client_id, *this);
}

void UIProcessConnectionFromClient::die()
{
    s_connections.remove(client_id());
}

void UIProcessConnectionFromClient::create_new_tab(Vector<ByteString> urls)
{
    if (on_new_tab)
        on_new_tab(sanitize_urls(urls, Application::settings().new_tab_page_url()));
}

void UIProcessConnectionFromClient::create_new_window(Vector<ByteString> urls)
{
    if (on_new_window)
        on_new_window(sanitize_urls(urls, Application::settings().new_tab_page_url()));
}

}
