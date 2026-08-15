/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebDriverBrowserConnection.h>
#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportBootstrapMach.h>
#else
#    include <LibCore/Socket.h>
#endif

namespace WebView {

ErrorOr<NonnullRefPtr<WebDriverBrowserConnection>> WebDriverBrowserConnection::connect(ByteString const& webdriver_endpoint)
{
#if defined(AK_OS_MACOS)
    auto transport_ports = TRY(IPC::bootstrap_transport_from_mach_server(webdriver_endpoint));
    auto transport = make<IPC::Transport>(move(transport_ports.receive_right), move(transport_ports.send_right));
#else
    auto socket = TRY(Core::LocalSocket::connect(webdriver_endpoint));
    auto transport = TRY(IPC::Transport::from_socket(move(socket)));
#endif
    return adopt_nonnull_ref_or_enomem(new (nothrow) WebDriverBrowserConnection(move(transport)));
}

WebDriverBrowserConnection::WebDriverBrowserConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebDriverBrowserClientEndpoint, WebDriverBrowserServerEndpoint>(*this, move(transport))
{
}

void WebDriverBrowserConnection::die()
{
    Application::the().webdriver_browser_connection_died({});
}

void WebDriverBrowserConnection::close_session()
{
    Core::EventLoop::current().quit(0);
}

}
