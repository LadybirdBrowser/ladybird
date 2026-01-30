/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <LibIPC/Transport.h>
#include <LibWebAudioWorkerClient/Client.h>

namespace WebAudioWorkerClient {

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<WebAudioWorkerClientEndpoint, WebAudioWorkerServerEndpoint>(*this, move(transport))
{
}

void Client::die()
{
    auto death_callback = move(on_death);
    on_death = {};

    if (death_callback)
        death_callback();
}

ErrorOr<IPC::File> Client::connect_new_webaudio_client_socket()
{
    auto socket = send_sync_but_allow_failure<Messages::WebAudioWorkerServer::ConnectNewWebaudioClient>();
    if (!socket)
        return Error::from_string_literal("Failed to connect to WebAudioWorker");

    auto file = socket->take_socket();
    TRY(file.clear_close_on_exec());
    return file;
}

}
