/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/VideoDecoderServerControlClient.h>

namespace WebView {

VideoDecoderServerControlClient::VideoDecoderServerControlClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<VideoDecoderServerControlClientEndpoint, VideoDecoderServerControlEndpoint>(*this, move(transport))
{
}

VideoDecoderServerControlClient::~VideoDecoderServerControlClient() = default;

void VideoDecoderServerControlClient::die()
{
    // The holder launches a new VideoDecoderServer when its MediaServer next asks for one; the MediaServer hears about
    // the loss from its own connection.
}

}
