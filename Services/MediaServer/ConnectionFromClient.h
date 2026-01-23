/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <MediaServer/Forward.h>
#include <MediaServer/MediaServerClientEndpoint.h>
#include <MediaServer/MediaServerServerEndpoint.h>

namespace MediaServer {

class ConnectionFromClient final
    : public IPC::ConnectionFromClient<MediaServerClientEndpoint, MediaServerServerEndpoint> {
    C_OBJECT(ConnectionFromClient);

public:
    ~ConnectionFromClient() override = default;

    void die() override;

private:
    explicit ConnectionFromClient(NonnullOwnPtr<IPC::Transport>);

    Messages::MediaServerServer::InitTransportResponse init_transport(int peer_pid) override;
    Messages::MediaServerServer::CreateSharedSingleProducerCircularBufferResponse create_shared_single_producer_circular_buffer(size_t capacity) override;
    Messages::MediaServerServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    static ErrorOr<IPC::File> connect_new_client();
};

}
