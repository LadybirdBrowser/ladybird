/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/ToAudioServerFromBrokerEndpoint.h>
#include <LibAudioServer/ToBrokerFromAudioServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>
#include <thread>

namespace Audio {

class BrokerConnection final
    : public IPC::ConnectionFromClient<ToBrokerFromAudioServerEndpoint, ToAudioServerFromBrokerEndpoint> {
    C_OBJECT(BrokerConnection);

public:
    ~BrokerConnection() override = default;

    void die() override;

private:
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }
    explicit BrokerConnection(NonnullOwnPtr<IPC::Transport>);

    Messages::ToAudioServerFromBroker::InitTransportResponse init_transport(int peer_pid) override;
    void revoke_grant(ByteString grant_id) override;
    Messages::ToAudioServerFromBroker::ConnectNewClientsResponse connect_new_clients(Vector<CreateClientRequest> requests) override;
    void connect_new_clients_async(u64 request_id, Vector<CreateClientRequest> requests) override;

    ErrorOr<Vector<CreateClientResponse>> create_new_clients(Vector<CreateClientRequest> requests);
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
