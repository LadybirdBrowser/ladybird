/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Socket.h>
#include <LibDevTools/Actors/RootActor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

using ActorRegistry = HashMap<ByteString, NonnullRefPtr<Actor>>;

class DevToolsServer {
public:
    static ErrorOr<NonnullOwnPtr<DevToolsServer>> create(DevToolsDelegate&, u16 port);
    ~DevToolsServer();

    RefPtr<Connection>& connection() { return m_connection; }
    DevToolsDelegate const& delegate() const { return m_delegate; }
    ActorRegistry const& actor_registry() const { return m_actor_registry; }

    template<typename ActorType, typename... Args>
    ActorType& register_actor(Args&&... args)
    {
        ByteString name;

        if constexpr (IsSame<ActorType, RootActor>) {
            name = ActorType::base_name;
        } else {
            name = ByteString::formatted("server{}-{}{}", m_server_id, ActorType::base_name, m_actor_count);
        }

        auto actor = ActorType::create(*this, name, forward<Args>(args)...);
        m_actor_registry.set(name, actor);
        ++m_actor_count;

        return actor;
    }

private:
    explicit DevToolsServer(DevToolsDelegate&, NonnullRefPtr<Core::TCPServer>);

    ErrorOr<void> on_new_client();
    void on_message_received(JsonObject const&);

    void close_connection();

    NonnullRefPtr<Core::TCPServer> m_server;
    RefPtr<Connection> m_connection;

    DevToolsDelegate& m_delegate;

    ActorRegistry m_actor_registry;
    RefPtr<RootActor> m_root_actor { nullptr };

    u64 m_server_id { 0 };
    u64 m_actor_count { 0 };
};

}
