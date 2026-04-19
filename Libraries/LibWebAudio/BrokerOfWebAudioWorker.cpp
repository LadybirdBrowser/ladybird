/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <LibIPC/Transport.h>
#include <LibWebAudio/BrokerOfWebAudioWorker.h>

namespace Web::WebAudio {

BrokerOfWebAudioWorker::BrokerOfWebAudioWorker(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToBrokerFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromBrokerEndpoint>(
          *this, move(transport))
{
}

void BrokerOfWebAudioWorker::shutdown()
{
    auto death_callback = move(on_death);
    on_death = {};

    if (death_callback)
        death_callback();
}

ErrorOr<IPC::TransportHandle> BrokerOfWebAudioWorker::connect_new_webaudio_client()
{
    auto response = send_sync_but_allow_failure<Messages::ToWebAudioWorkerFromBroker::ConnectNewWebaudioClient>();
    if (!response)
        return Error::from_string_literal("Failed to connect to WebAudioWorker");

    return response->take_handle();
}

} // namespace Web::WebAudio
