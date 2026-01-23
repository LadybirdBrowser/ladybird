/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibIPC/ConnectionToServer.h>
#include <MediaServer/MediaServerClientEndpoint.h>
#include <MediaServer/MediaServerServerEndpoint.h>

namespace MediaServerClient {

class Client final
    : public IPC::ConnectionToServer<MediaServerClientEndpoint, MediaServerServerEndpoint>
    , public MediaServerClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::MediaServerServer::InitTransport;

    explicit Client(NonnullOwnPtr<IPC::Transport>);

    ErrorOr<Core::SharedSingleProducerCircularBuffer> create_shared_single_producer_circular_buffer(size_t capacity);

private:
    void die() override;
};

}
