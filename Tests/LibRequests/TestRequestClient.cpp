/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>
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

// LeakSanitizer reports what a joined thread allocated to read a response body and did not free.
TEST_CASE(a_thread_that_read_a_response_body_can_exit)
{
    IGNORE_USE_IN_ESCAPING_LAMBDA ByteString received_body;
    auto thread = Threading::Thread::construct("ResponseReader"sv, [&received_body] {
        Core::EventLoop event_loop;

        auto connected = connect_client(1);
        auto request_server = MUST(connected.request_server.create_transport());
        auto request = start_request(*connected.client);
        bool finished = false;
        request->set_buffered_request_finished_callback([&](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const&, NonnullRefPtr<HTTP::HeaderList>, Optional<u32>, Optional<String>, Requests::CacheState, Core::ImmutableBytes payload) {
            received_body = ByteString { payload.bytes() };
            finished = true;
        });

        int body_fds[2] {};
        MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, body_fds));
        MUST(Core::System::set_socket_blocking(body_fds[0], false));
        auto body = "body"sv;
        MUST(Core::System::send(body_fds[1], body.bytes(), MSG_NOSIGNAL));
        MUST(Core::System::close(body_fds[1]));

        auto send = [&](IPC::MessageBuffer message) { MUST(message.transfer_message(*request_server)); };
        send(MUST(Messages::RequestClient::RequestStarted::static_encode(request->id(), IPC::File::adopt_fd(body_fds[0]))));
        send(MUST(Messages::RequestClient::HeadersBecameAvailable::static_encode(request->id(), URL::about_blank(), Vector<HTTP::Header> {}, 200, Optional<String> {}, Requests::CacheState::NotCached, ByteString {})));
        send(MUST(Messages::RequestClient::RequestFinished::static_encode(request->id(), body.length(), Requests::RequestTimingInfo {}, Optional<Requests::NetworkError> {})));

        event_loop.spin_until([&] { return finished; });
        return static_cast<intptr_t>(0);
    });
    thread->start();
    MUST(thread->join());

    EXPECT_EQ(received_body, "body"sv);
}
