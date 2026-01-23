/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMediaServerClient/Client.h>

namespace MediaServerClient {

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<MediaServerClientEndpoint, MediaServerServerEndpoint>(*this, move(transport))
{
}

void Client::die()
{
}

ErrorOr<Core::SharedSingleProducerCircularBuffer> Client::create_shared_single_producer_circular_buffer(size_t capacity)
{
    auto response = send_sync_but_allow_failure<Messages::MediaServerServer::CreateSharedSingleProducerCircularBuffer>(capacity);
    if (!response)
        return Error::from_string_literal("MediaServerClient: create buffer IPC failed");

    auto buffer = response->shm_buffer();
    if (!buffer.is_valid())
        return Error::from_string_literal("MediaServerClient: server returned invalid buffer");

    return Core::SharedSingleProducerCircularBuffer::attach(move(buffer));
}

}
