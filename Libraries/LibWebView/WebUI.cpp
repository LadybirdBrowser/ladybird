/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WebUI/ProcessesUI.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

template<typename WebUIType>
static ErrorOr<NonnullRefPtr<WebUIType>> create_web_ui(WebContentClient& client, String host)
{
    Array<int, 2> socket_fds { 0, 0 };
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds.data()));

    auto client_socket = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket.is_error()) {
        close(socket_fds[0]);
        close(socket_fds[1]);

        return client_socket.release_error();
    }

    auto web_ui = WebUIType::create(client, IPC::Transport { client_socket.release_value() }, move(host));
    client.async_connect_to_web_ui(0, IPC::File::adopt_fd(socket_fds[1]));

    return web_ui;
}

ErrorOr<RefPtr<WebUI>> WebUI::create(WebContentClient& client, String host)
{
    RefPtr<WebUI> web_ui;

    if (host == "processes"sv)
        web_ui = TRY(create_web_ui<ProcessesUI>(client, move(host)));
    else if (host == "settings"sv)
        web_ui = TRY(create_web_ui<SettingsUI>(client, move(host)));

    if (web_ui)
        web_ui->register_interfaces();

    return web_ui;
}

WebUI::WebUI(WebContentClient& client, IPC::Transport transport, String host)
    : IPC::ConnectionToServer<WebUIClientEndpoint, WebUIServerEndpoint>(*this, move(transport))
    , m_client(client)
    , m_host(move(host))
{
}

WebUI::~WebUI() = default;

void WebUI::die()
{
    m_client.web_ui_disconnected({});
}

void WebUI::register_interface(StringView name, Interface interface)
{
    auto result = m_interfaces.set(name, move(interface));
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void WebUI::received_message(String name, JsonValue data)
{
    auto interface = m_interfaces.get(name);
    if (!interface.has_value()) {
        warnln("Received message from WebUI for unrecognized interface: {}", name);
        return;
    }

    interface.value()(move(data));
}

}
