/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/BrokerConnection.h>
#include <AudioServer/Server.h>
#include <AudioServer/SessionConnection.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>

namespace AudioServer {

using namespace Messages::ToAudioServerFromBroker;

static constexpr int s_broker_client_id = 1;

BrokerConnection::BrokerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToBrokerFromAudioServerEndpoint, ToAudioServerFromBrokerEndpoint>(*this, move(transport), s_broker_client_id)
{
}

void BrokerConnection::die()
{
    verify_thread_affinity();
    Core::EventLoop::current().quit(0);
}

InitTransportResponse BrokerConnection::init_transport([[maybe_unused]] int peer_pid)
{
    verify_thread_affinity();
    return { Core::System::getpid() };
}

void BrokerConnection::revoke_grant(ByteString grant_id)
{
    verify_thread_affinity();
    Server::the().revoke_grant(grant_id);
    Server::the().revoke_grant_on_all_sessions(grant_id);
}

ErrorOr<Vector<AudioServer::CreateClientResponse>> BrokerConnection::create_new_clients(Vector<AudioServer::CreateClientRequest> requests)
{
    Vector<AudioServer::CreateClientResponse> responses;
    responses.ensure_capacity(requests.size());

    for (auto& request : requests) {
        auto file_or_error = SessionConnection::connect_new_client_for_broker();
        if (file_or_error.is_error())
            return file_or_error.release_error();

        responses.unchecked_append(AudioServer::CreateClientResponse {
            .socket = file_or_error.release_value(),
            .grant_id = Server::the().create_grant(move(request.origin), move(request.top_level_origin), request.can_use_mic),
        });
    }

    return responses;
}

ConnectNewClientsResponse BrokerConnection::connect_new_clients(Vector<AudioServer::CreateClientRequest> requests)
{
    verify_thread_affinity();
    auto responses_or_error = create_new_clients(move(requests));
    if (responses_or_error.is_error())
        return Vector<AudioServer::CreateClientResponse> {};
    return { responses_or_error.release_value() };
}

void BrokerConnection::connect_new_clients_async(u64 request_token, Vector<AudioServer::CreateClientRequest> requests)
{
    verify_thread_affinity();
    auto responses_or_error = create_new_clients(move(requests));
    if (responses_or_error.is_error()) {
        async_did_fail_to_connect_new_clients(request_token, ByteString("AudioServer: connect_new_clients_async failed"));
        return;
    }

    async_did_connect_new_clients(request_token, responses_or_error.release_value());
}

}
