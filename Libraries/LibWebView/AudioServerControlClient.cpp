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

void AudioServerControlClient::capture_requested(u64 tab_id, u64 request_id, ByteString device_id)
{
    if (on_capture_requested)
        on_capture_requested(tab_id, request_id, device_id);
}

}
