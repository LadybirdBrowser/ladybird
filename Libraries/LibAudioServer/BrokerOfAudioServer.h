/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/ToAudioServerFromBrokerEndpoint.h>
#include <LibAudioServer/ToBrokerFromAudioServerEndpoint.h>
#include <LibIPC/ConnectionToServer.h>
#include <thread>

namespace AudioServer {

class BrokerOfAudioServer final
    : public IPC::ConnectionToServer<ToBrokerFromAudioServerEndpoint, ToAudioServerFromBrokerEndpoint>
    , public ToBrokerFromAudioServerEndpoint {
    C_OBJECT_ABSTRACT(BrokerOfAudioServer);

public:
    using InitTransport = Messages::ToAudioServerFromBroker::InitTransport;

    explicit BrokerOfAudioServer(NonnullOwnPtr<IPC::Transport>);

    void revoke_grant(ByteString grant_id);
    ErrorOr<AudioServer::CreateClientResponse> connect_new_client(StringView origin = "*"sv, StringView top_level_origin = "*"sv, bool can_use_mic = false);
    ErrorOr<Vector<AudioServer::CreateClientResponse>> connect_new_clients(Vector<AudioServer::CreateClientRequest> requests);
    ErrorOr<void> connect_new_clients_async(Vector<AudioServer::CreateClientRequest>&& requests, Function<void(ErrorOr<Vector<AudioServer::CreateClientResponse>>)> callback);

    Function<void()> on_death;

private:
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }
    u64 next_request_token();
    void die() override;
    void did_connect_new_clients(u64 request_token, Vector<AudioServer::CreateClientResponse> responses) override;
    void did_fail_to_connect_new_clients(u64 request_token, ByteString error) override;

    u64 m_next_request_token { 1 };
    HashMap<u64, Function<void(ErrorOr<Vector<AudioServer::CreateClientResponse>>)>> m_pending_connect_new_clients_callbacks;
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
