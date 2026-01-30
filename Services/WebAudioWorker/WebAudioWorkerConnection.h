/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ConnectionFromClient.h>
#include <WebAudioWorker/WebAudioWorkerClientEndpoint.h>
#include <WebAudioWorker/WebAudioWorkerServerEndpoint.h>

namespace WebAudioWorker {

class WebAudioWorkerConnection final
    : public IPC::ConnectionFromClient<WebAudioWorkerClientEndpoint, WebAudioWorkerServerEndpoint> {
    C_OBJECT(WebAudioWorkerConnection);

public:
    ~WebAudioWorkerConnection() override;

    void die() override;

    static bool has_any_connection();
    static void maybe_quit_event_loop_if_unused();

private:
    explicit WebAudioWorkerConnection(NonnullOwnPtr<IPC::Transport>);

    Messages::WebAudioWorkerServer::InitTransportResponse init_transport(int peer_pid) override;

    Messages::WebAudioWorkerServer::ConnectNewWebaudioClientResponse connect_new_webaudio_client() override;
    Messages::WebAudioWorkerServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    ErrorOr<IPC::File> connect_new_webaudio_client_impl();
    static ErrorOr<IPC::File> connect_new_client();
};

}
