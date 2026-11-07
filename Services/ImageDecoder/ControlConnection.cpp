/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/ConnectionFromClient.h>
#include <ImageDecoder/ControlConnection.h>

namespace ImageDecoder {

ControlConnection::ControlConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ImageDecoderControlClientEndpoint, ImageDecoderControlEndpoint>(*this, move(transport), 0)
{
    ImageDecoder::ConnectionFromClient::set_is_held_open_by_browser(ImageDecoder::ConnectionFromClient::IsHeldOpenByBrowser::Yes);
}

ControlConnection::~ControlConnection() = default;

void ControlConnection::die()
{
    ImageDecoder::ConnectionFromClient::set_is_held_open_by_browser(ImageDecoder::ConnectionFromClient::IsHeldOpenByBrowser::No);
}

void ControlConnection::connect_new_client(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("ImageDecoder: Could not take up a client: {}", transport_or_error.error());
        return;
    }
    // NB: The new connection keeps itself alive until it dies.
    (void)ImageDecoder::ConnectionFromClient::construct(transport_or_error.release_value());
}

}
