/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/ConnectionToServer.h>
#include <MediaServer/MediaClientEndpoint.h>
#include <MediaServer/MediaServerEndpoint.h>

namespace MediaClient {

class Client final
    : public IPC::ConnectionToServer<MediaClientEndpoint, MediaServerEndpoint>
    , public MediaClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::MediaServer::InitTransport;
    using TransportFactory = Function<ErrorOr<NonnullOwnPtr<IPC::Transport>>()>;

    // Installed by the process that reaches the Browser, which spawns the process's MediaServer and brokers the
    // connection to it on request.
    static void set_transport_factory(TransportFactory);

    // The process's connection to its MediaServer, established on first use.
    static ErrorOr<NonnullRefPtr<Client>> acquire();

    explicit Client(NonnullOwnPtr<IPC::Transport>);
    virtual ~Client() override;

    Function<void()> on_death;

private:
    virtual void die() override;

    void verify_event_loop() const;

    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
};

}
