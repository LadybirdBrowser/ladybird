/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Process.h>
#include <LibMediaCodecs/VideoDecoderServerConnection.h>
#include <VideoDecoderServer/ControlConnection.h>

namespace VideoDecoderServer {

ControlConnection::ControlConnection(NonnullOwnPtr<IPC::Transport> transport, ReadonlySpan<Media::VideoDecoderRegistration> hosted_decoders)
    : IPC::ConnectionFromClient<VideoDecoderServerControlClientEndpoint, VideoDecoderServerControlEndpoint>(*this, move(transport), 0)
    , m_hosted_decoders(hosted_decoders)
{
}

ControlConnection::~ControlConnection() = default;

void ControlConnection::die()
{
    Core::Process::terminate_immediately(0);
}

Messages::VideoDecoderServerControl::ConnectNewClientResponse ControlConnection::connect_new_client()
{
    if (m_client) {
        did_misbehave("The video decoder server serves only one client");
        return OptionalNone {};
    }

    auto paired_transports = IPC::Transport::create_paired();
    if (paired_transports.is_error()) {
        dbgln("Failed to connect a video decoder client: {}", paired_transports.error());
        return OptionalNone {};
    }

    // Once its client is gone, the server has nothing left to do.
    m_client = Media::VideoDecoderServerConnection::construct(move(paired_transports.value().local), m_hosted_decoders);
    m_client->on_death = [] {
        Core::Process::terminate_immediately(0);
    };
    return move(paired_transports.value().remote_handle);
}

}
