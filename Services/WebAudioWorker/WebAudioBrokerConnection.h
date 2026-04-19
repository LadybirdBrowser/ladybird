/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWebAudio/ToBrokerFromWebAudioWorkerEndpoint.h>
#include <LibWebAudio/ToWebAudioWorkerFromBrokerEndpoint.h>

namespace Web::WebAudio {

class WebAudioBrokerConnection final : public IPC::ConnectionFromClient<ToBrokerFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromBrokerEndpoint> {
    C_OBJECT(WebAudioBrokerConnection);

public:
    ~WebAudioBrokerConnection() override;

    void die() override { shutdown(); }
    void shutdown();

    static bool has_any_connection();
    static void maybe_quit_event_loop_if_unused();

private:
    explicit WebAudioBrokerConnection(NonnullOwnPtr<IPC::Transport>);

    Messages::ToWebAudioWorkerFromBroker::InitTransportResponse init_transport(int peer_pid) override;
    void connect_to_audio_server(IPC::TransportHandle handle, ByteString grant_id) override;

    Messages::ToWebAudioWorkerFromBroker::ConnectNewWebaudioClientResponse
    connect_new_webaudio_client() override;
    Messages::ToWebAudioWorkerFromBroker::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    static ErrorOr<IPC::TransportHandle> connect_new_client();
};

} // namespace Web::WebAudio
