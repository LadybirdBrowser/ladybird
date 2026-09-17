/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

TEST_CASE(non_windows_transport_initialization_preserves_other_clients)
{
    MUST(RequestServer::initialize_libcurl());
    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;

    auto attacker_pair = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto attacker_remote = TRY_OR_FAIL(attacker_pair.remote_handle.create_transport());
    auto attacker = RequestServer::ConnectionFromClient::construct(move(attacker_pair.local), RequestServer::IsPrivate::No, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {}, ByteString {});

    auto other_pair = TRY_OR_FAIL(IPC::Transport::create_paired());
    auto other_remote = TRY_OR_FAIL(other_pair.remote_handle.create_transport());
    auto other = RequestServer::ConnectionFromClient::construct(move(other_pair.local), RequestServer::IsPrivate::No, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {}, ByteString {});
    EXPECT_EQ(connections.size(), 2u);

    auto message = make<Messages::RequestServer::InitTransport>(42);
    (void)MUST(static_cast<RequestServerEndpoint::Stub&>(*attacker).handle(move(message)));

    EXPECT(!attacker->is_open());
    EXPECT_EQ(connections.size(), 1u);
    EXPECT(other->is_open());
}
