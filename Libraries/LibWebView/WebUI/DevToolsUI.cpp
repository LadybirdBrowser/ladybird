/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/CallbackTransport.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWebView/Application.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebUI/DevToolsUI.h>

namespace WebView {

ErrorOr<NonnullRefPtr<DevToolsUI>> DevToolsUI::create_with_inspected_view_id(WebContentClient& client, String host, Optional<u64> inspected_view_id)
{
    auto paired = TRY(IPC::Transport::create_paired());
    auto handle = move(paired.remote_handle);

    auto web_ui = adopt_ref(*new DevToolsUI(client, move(paired.local), move(host), inspected_view_id));
    client.async_connect_to_web_ui(0, move(handle));

    return web_ui;
}

DevToolsUI::DevToolsUI(WebContentClient& client, NonnullOwnPtr<IPC::Transport> transport, String host, Optional<u64> inspected_view_id)
    : WebUI(client, move(transport), move(host))
    , m_inspected_view_id(inspected_view_id)
{
}

DevToolsUI::~DevToolsUI()
{
    m_alive = false;
}

void DevToolsUI::register_interfaces()
{
    auto& app = Application::the();

    if (!app.devtools_server())
        app.ensure_devtools_server();

    app.devtools_server()->connect_pipe([this](JsonValue const& message) {
        if (!m_alive)
            return;
        async_send_message("devtools.receive"sv, message);
    });

    register_interface("devtools.send"sv, [](auto data) {
        if (!data.is_object())
            return;
        auto* server = Application::the().devtools_server();
        if (!server)
            return;
        auto& transport = server->connection();
        if (transport && transport->on_message_received)
            transport->on_message_received(data.as_object());
    });

    register_interface("devtools.getInspectedBrowserId"sv, [this](auto const&) {
        get_inspected_browser_id();
    });
    register_interface("devtools.loadRemoteDebuggingSettings"sv, [this](auto const&) {
        load_remote_debugging_settings();
    });
    register_interface("devtools.setRemoteDebuggingSettings"sv, [this](auto const& data) {
        set_remote_debugging_settings(data);
    });
}

void DevToolsUI::get_inspected_browser_id()
{
    async_send_message("devtools.inspectedBrowserId"sv, m_inspected_view_id.has_value() ? JsonValue(*m_inspected_view_id) : JsonValue {});
}

void DevToolsUI::load_remote_debugging_settings()
{
    auto settings = Application::settings().serialize_json();
    auto remote_debugging = settings.as_object().get("remoteDebugging"sv).value_or(JsonValue(JsonObject {}));
    async_send_message("devtools.remoteDebuggingSettings"sv, move(remote_debugging));
}

void DevToolsUI::set_remote_debugging_settings(JsonValue const& remote_debugging_settings)
{
    if (!remote_debugging_settings.is_object())
        return;

    auto enabled = remote_debugging_settings.as_object().get_bool("enabled"sv);
    auto port = remote_debugging_settings.as_object().get_u16("port"sv);
    if (!enabled.has_value() || !port.has_value())
        return;

    Application::settings().set_remote_debugging_settings({
        .enabled = *enabled,
        .port = *port,
    });
    load_remote_debugging_settings();
}

}
