/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibIPC/Transport.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

namespace {

// A client whose RequestServer end of the connection is held, and never read.
struct ConnectedClient {
    NonnullRefPtr<Requests::RequestClient> client;
    IPC::TransportHandle request_server;
};

}

static ConnectedClient connect_client(int client_id)
{
    auto pair = MUST(IPC::Transport::create_paired());
    auto client = make_ref_counted<Requests::RequestClient>(move(pair.local), client_id);
    return { move(client), move(pair.remote_handle) };
}

static NonnullRefPtr<Requests::Request> start_request(Requests::RequestClient& client)
{
    auto url = URL::Parser::basic_parse("https://example.com/"sv);
    VERIFY(url.has_value());
    auto request = client.start_request("GET"sv, *url, HTTP::Cookie::SameSiteContext {});
    VERIFY(request);
    return request.release_nonnull();
}

TEST_CASE(requests_of_two_clients_with_the_same_id_have_different_transport_epochs)
{
    Core::EventLoop event_loop;

    // RequestServer can give a replacement client the id of the client it replaces.
    auto first = connect_client(1);
    auto replacement = connect_client(1);
    auto other = connect_client(2);

    auto first_request = start_request(*first.client);
    auto replacement_request = start_request(*replacement.client);
    EXPECT_EQ(first_request->id(), replacement_request->id());

    EXPECT_EQ(first_request->transport_epoch(), first.client->transport_epoch());
    EXPECT_EQ(replacement_request->transport_epoch(), replacement.client->transport_epoch());
    EXPECT_NE(first.client->transport_epoch(), replacement.client->transport_epoch());
    EXPECT_NE(other.client->transport_epoch(), first.client->transport_epoch());
    EXPECT_NE(other.client->transport_epoch(), replacement.client->transport_epoch());
}

TEST_CASE(known_hsts_host_lookups_answer_that_the_host_is_known_after_request_server_dies)
{
    Core::EventLoop event_loop;

    auto connected = connect_client(1);
    Vector<StringView> events;
    connected.client->on_request_server_died = [&] { events.append("died"sv); };
    connected.client->look_up_known_hsts_host("example.com"_string, [&](bool is_known) {
        events.append(is_known ? "known"sv : "not known"sv);
    });

    (void)MUST(connected.request_server.create_transport());

    event_loop.spin_until([&] { return events.size() == 2; });
    EXPECT_EQ(events, (Vector<StringView> { "died"sv, "known"sv }));
}

TEST_CASE(known_hsts_host_lookups_made_after_request_server_dies_answer_that_the_host_is_known)
{
    Core::EventLoop event_loop;

    auto connected = connect_client(1);
    Vector<StringView> events;
    connected.client->on_request_server_died = [&] { events.append("died"sv); };

    (void)MUST(connected.request_server.create_transport());
    event_loop.spin_until([&] { return events.size() == 1; });

    connected.client->look_up_known_hsts_host("example.com"_string, [&](bool is_known) {
        events.append(is_known ? "known"sv : "not known"sv);
    });

    event_loop.spin_until([&] { return events.size() == 2; });
    EXPECT_EQ(events, (Vector<StringView> { "died"sv, "known"sv }));
}

TEST_CASE(a_request_keeps_its_transport_epoch_once_its_client_is_gone)
{
    Core::EventLoop event_loop;

    RefPtr<Requests::Request> request;
    u64 epoch = 0;
    {
        auto connected = connect_client(1);
        epoch = connected.client->transport_epoch();
        request = start_request(*connected.client);
    }
    EXPECT_EQ(request->transport_epoch(), epoch);
}
