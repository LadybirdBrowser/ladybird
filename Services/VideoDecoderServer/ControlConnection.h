/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <LibMediaCodecs/Forward.h>
#include <LibMediaCodecs/VideoDecoderRegistry.h>
#include <VideoDecoderServer/VideoDecoderServerControlClientEndpoint.h>
#include <VideoDecoderServer/VideoDecoderServerControlEndpoint.h>

namespace VideoDecoderServer {

// The control plane of the VideoDecoderServer, spoken only by the Browser over the initial socket. It connects the one
// client the server serves; the server exits when either the Browser or that client goes away.
class ControlConnection final
    : public IPC::ConnectionFromClient<VideoDecoderServerControlClientEndpoint, VideoDecoderServerControlEndpoint> {
    C_OBJECT(ControlConnection);

public:
    virtual ~ControlConnection() override;

    virtual void die() override;

private:
    ControlConnection(NonnullOwnPtr<IPC::Transport>, ReadonlySpan<Media::VideoDecoderRegistration> hosted_decoders);

    virtual Messages::VideoDecoderServerControl::ConnectNewClientResponse connect_new_client() override;

    ReadonlySpan<Media::VideoDecoderRegistration> const m_hosted_decoders;
    RefPtr<Media::VideoDecoderServerConnection> m_client;
};

}
