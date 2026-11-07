/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/ImageDecoderControlClient.h>

namespace WebView {

ImageDecoderControlClient::ImageDecoderControlClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ImageDecoderControlClientEndpoint, ImageDecoderControlEndpoint>(*this, move(transport))
{
}

ImageDecoderControlClient::~ImageDecoderControlClient() = default;

void ImageDecoderControlClient::die()
{
    // The clients of a decoder that died hear about it from their own connections, and ask for a new one when they next
    // have an image to decode.
}

}
