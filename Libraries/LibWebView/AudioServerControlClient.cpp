/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/AudioServerControlClient.h>

namespace WebView {

AudioServerControlClient::AudioServerControlClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<AudioServerControlClientEndpoint, AudioServerControlEndpoint>(*this, move(transport))
{
}

AudioServerControlClient::~AudioServerControlClient() = default;

void AudioServerControlClient::die()
{
    // The holder launches a new AudioServer when it next connects a client; the clients of this one hear about it from
    // their own connections.
}

}
