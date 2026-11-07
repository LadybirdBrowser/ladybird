/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <ImageDecoder/ImageDecoderControlClientEndpoint.h>
#include <ImageDecoder/ImageDecoderControlEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>

namespace ImageDecoder {

// The control plane of the ImageDecoder, spoken only by the Browser over the initial socket. It connects the clients the
// decoder serves, and tells the decoder nothing of what they decode. Once the Browser closes it, the decoder exits with
// the last of its clients.
class ControlConnection final
    : public IPC::ConnectionFromClient<ImageDecoderControlClientEndpoint, ImageDecoderControlEndpoint> {
    C_OBJECT(ControlConnection);

public:
    virtual ~ControlConnection() override;

    virtual void die() override;

private:
    explicit ControlConnection(NonnullOwnPtr<IPC::Transport>);

    virtual void connect_new_client(IPC::TransportHandle) override;
};

}
