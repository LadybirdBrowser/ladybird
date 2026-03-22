/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#endif
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>

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
#if defined(AK_OS_MACOS)
        TRY(connect_as_client(*pid, raw_urls, new_window));
#else
        TRY(connect_as_client(socket_path, raw_urls, new_window));
#endif
        return ProcessDisposition::ExitProcess;
    }

#if defined(AK_OS_MACOS)
    TRY(connect_as_server());
#else
    TRY(connect_as_server(socket_path));
#endif

    m_pid_path = pid_path;
    m_pid_file = TRY(Core::File::open(pid_path, Core::File::OpenMode::Write));
    TRY(m_pid_file->write_until_depleted(ByteString::number(Core::System::getpid())));

    return ProcessDisposition::ContinueMainProcess;
}

#if defined(AK_OS_MACOS)
ErrorOr<void> BrowserProcess::connect_as_client(pid_t pid, Vector<ByteString> const& raw_urls, NewWindow new_window)
{
    auto transport_ports = TRY(IPC::bootstrap_transport_from_mach_server(mach_server_name_for_process("Ladybird"sv, pid)));
    auto client = UIProcessClient::construct(make<IPC::Transport>(move(transport_ports.receive_right), move(transport_ports.send_right)));

    switch (new_window) {
    case NewWindow::Yes:
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewWindow>(raw_urls))
            dbgln("Failed to send CreateNewWindow message to UIProcess");
        return {};
    case NewWindow::No:
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewTab>(raw_urls))
            dbgln("Failed to send CreateNewTab message to UIProcess");
        return {};
    }

    VERIFY_NOT_REACHED();
}
#else
ErrorOr<void> BrowserProcess::connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, NewWindow new_window)
{
    auto socket = TRY(Core::LocalSocket::connect(socket_path));
    auto transport = TRY(IPC::Transport::from_socket(move(socket)));
    auto client = UIProcessClient::construct(move(transport));

    switch (new_window) {
    case NewWindow::Yes:
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewWindow>(raw_urls))
            dbgln("Failed to send CreateNewWindow message to UIProcess");
        return {};
    case NewWindow::No:
        if (!client->send_sync_but_allow_failure<Messages::UIProcessServer::CreateNewTab>(raw_urls))
            dbgln("Failed to send CreateNewTab message to UIProcess");
        return {};
    }

    VERIFY_NOT_REACHED();
}
#endif

#if defined(AK_OS_MACOS)
ErrorOr<void> BrowserProcess::connect_as_server()
{
    Application::the().set_browser_process_transport_handler([this](auto transport) {
        accept_transport(move(transport));
    });
    return {};
}
#else
ErrorOr<void> BrowserProcess::connect_as_server(ByteString const& socket_path)
{
    auto socket_fd = TRY(Process::create_ipc_socket(socket_path));
    m_socket_path = socket_path;
    m_local_server = Core::LocalServer::construct();
    TRY(m_local_server->take_over_fd(socket_fd));

    m_local_server->on_accept = [this](auto client_socket) {
        auto transport = IPC::Transport::from_socket(move(client_socket));
        if (transport.is_error()) {
            dbgln("Failed to create IPC transport for UIProcess client: {}", transport.error());
            return;
        }

        accept_transport(transport.release_value());
    };

    return {};
}
#endif

void BrowserProcess::accept_transport(NonnullOwnPtr<IPC::Transport> transport)
{
    auto client = UIProcessConnectionFromClient::construct(move(transport), ++m_next_client_id);
    client->on_new_tab = [this](auto raw_urls) {
        if (this->on_new_tab)
            this->on_new_tab(raw_urls);
    };

    client->on_new_window = [this](auto raw_urls) {
        if (this->on_new_window)
            this->on_new_window(raw_urls);
    };
}

BrowserProcess::~BrowserProcess()
{
#if defined(AK_OS_MACOS)
    Application::the().set_browser_process_transport_handler({});
#endif

    if (m_pid_file) {
        MUST(m_pid_file->truncate(0));
#if defined(AK_OS_WINDOWS)
        // NOTE: On Windows, System::open() duplicates the underlying OS file handle,
        // so we need to explicitly close said handle, otherwise the unlink() call fails due
        // to permission errors and we crash on shutdown.
        m_pid_file->close();
#endif
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
