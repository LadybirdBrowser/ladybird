/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibDevTools/Actors/DeviceActor.h>
#include <LibDevTools/Actors/PreferenceActor.h>
#include <LibDevTools/Actors/ProcessActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Connection.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static u64 s_server_count = 0;

ErrorOr<NonnullOwnPtr<DevToolsServer>> DevToolsServer::create(DevToolsDelegate& delegate, u16 port)
{
    auto address = IPv4Address::from_string("0.0.0.0"sv).release_value();

    auto server = TRY(Core::TCPServer::try_create());
    TRY(server->listen(address, port, Core::TCPServer::AllowAddressReuse::Yes));

    return adopt_own(*new DevToolsServer(delegate, move(server)));
}

DevToolsServer::DevToolsServer(DevToolsDelegate& delegate, NonnullRefPtr<Core::TCPServer> server)
    : m_server(move(server))
    , m_delegate(delegate)
    , m_server_id(s_server_count++)
{
    m_server->on_ready_to_accept = [this]() {
        if (auto result = on_new_client(); result.is_error())
            warnln("Failed to accept DevTools client: {}", result.error());
    };
}

DevToolsServer::~DevToolsServer() = default;

void DevToolsServer::refresh_tab_list()
{
    if (!m_root_actor)
        return;

    m_actor_registry.remove_all_matching([](auto const&, auto const& actor) {
        return is<TabActor>(*actor);
    });

    m_root_actor->send_tab_list_changed_message();
}

ErrorOr<void> DevToolsServer::on_new_client()
{
    if (m_connection)
        return Error::from_string_literal("Only one active DevTools connection is currently allowed");

    auto client = TRY(m_server->accept());
    auto buffered_socket = TRY(Core::BufferedTCPSocket::create(move(client)));

    m_connection = Connection::create(move(buffered_socket));

    m_connection->on_connection_closed = [this]() {
        close_connection();
    };

    m_connection->on_message_received = [this](auto const& message) {
        on_message_received(message);
    };

    m_root_actor = register_actor<RootActor>();

    register_actor<DeviceActor>();
    register_actor<PreferenceActor>();
    register_actor<ProcessActor>(ProcessDescription { .is_parent = true });

    return {};
}

void DevToolsServer::on_message_received(JsonObject const& message)
{
    auto to = message.get_byte_string("to"sv);
    if (!to.has_value()) {
        m_root_actor->send_missing_parameter_error("to"sv);
        return;
    }

    auto actor = m_actor_registry.find(*to);
    if (actor == m_actor_registry.end()) {
        m_root_actor->send_unknown_actor_error(*to);
        return;
    }

    auto type = message.get_byte_string("type"sv);
    if (!type.has_value()) {
        actor->value->send_missing_parameter_error("type"sv);
        return;
    }

    actor->value->handle_message(*type, message);
}

void DevToolsServer::close_connection()
{
    dbgln_if(DEVTOOLS_DEBUG, "Lost connection to the DevTools client");

    Core::deferred_invoke([this]() {
        m_connection = nullptr;
        m_actor_registry.clear();
        m_root_actor = nullptr;
    });
}

}
