/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServer/BrokerOfAudioServer.h>

namespace Audio {

using namespace Messages::ToAudioServerFromBroker;

BrokerOfAudioServer::BrokerOfAudioServer(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToBrokerFromAudioServerEndpoint, ToAudioServerFromBrokerEndpoint>(*this, move(transport))
{
}

void BrokerOfAudioServer::die()
{
    verify_thread_affinity();
    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

void BrokerOfAudioServer::revoke_grant(ByteString grant_id)
{
    verify_thread_affinity();
    (void)post_message(RevokeGrant(move(grant_id)));
}

ErrorOr<CreateClientResponse> BrokerOfAudioServer::connect_new_client(StringView origin, StringView top_level_origin, bool can_use_mic)
{
    Vector<CreateClientRequest> requests;
    TRY(requests.try_append({ .origin = origin, .top_level_origin = top_level_origin, .can_use_mic = can_use_mic }));

    auto responses = TRY(connect_new_clients(move(requests)));
    if (responses.size() != 1)
        return Error::from_string_literal("AudioServerClient: connect new client IPC returned unexpected count");

    return responses.take_last();
}

ErrorOr<Vector<CreateClientResponse>> BrokerOfAudioServer::connect_new_clients(Vector<CreateClientRequest> requests)
{
    verify_thread_affinity();
    auto response = send_sync_but_allow_failure<ConnectNewClients>(move(requests));
    if (!response)
        return Error::from_string_literal("AudioServerClient: connect new clients IPC failed");
    return response->take_responses();
}

u64 BrokerOfAudioServer::next_request_id()
{
    verify_thread_affinity();
    return m_next_request_id++;
}

ErrorOr<void> BrokerOfAudioServer::connect_new_clients_async(Vector<CreateClientRequest>&& requests, Function<void(ErrorOr<Vector<CreateClientResponse>>)> callback)
{
    verify_thread_affinity();

    u64 request_id = next_request_id();
    TRY(m_pending_connect_new_clients_callbacks.try_set(request_id, move(callback)));
    async_connect_new_clients_async(request_id, move(requests));
    return {};
}

void BrokerOfAudioServer::did_connect_new_clients(u64 request_id, Vector<CreateClientResponse> responses)
{
    verify_thread_affinity();
    auto callback = m_pending_connect_new_clients_callbacks.take(request_id);

    if (!callback.has_value())
        return;
    callback.value()(move(responses));
}

void BrokerOfAudioServer::did_fail_to_connect_new_clients(u64 request_id, ByteString error)
{
    verify_thread_affinity();

    auto callback = m_pending_connect_new_clients_callbacks.take(request_id);

    if (!callback.has_value())
        return;

    (void)error;
    callback.value()(Error::from_string_literal("AudioServerClient: async connect new clients IPC failed"));
}

}
