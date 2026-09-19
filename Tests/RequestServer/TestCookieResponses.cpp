/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ControlConnectionFromClient.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

namespace {

struct TestServer {
    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
};

static void initialize_libcurl()
{
    static bool libcurl_initialized = [] {
        MUST(RequestServer::initialize_libcurl());
        return true;
    }();
    (void)libcurl_initialized;
}

// The initial socket: the only connection that may answer cookie lookups.
class TestControlConnection {
public:
    explicit TestControlConnection(TestServer& server)
    {
        initialize_libcurl();

        auto pair = MUST(IPC::Transport::create_paired());
        m_remote_transport = MUST(pair.remote_handle.create_transport());
        m_connection = RequestServer::ControlConnectionFromClient::construct(
            move(pair.local), server.connections, server.request_transfer_leases,
            Optional<HTTP::DiskCache&> {}, ByteString {});
#ifdef AK_OS_WINDOWS
        auto pid = Core::System::getpid();
        m_connection->transport().set_peer_pid(pid);
        m_remote_transport->set_peer_pid(pid);
#endif
    }

    ~TestControlConnection()
    {
        if (m_connection->is_open())
            m_connection->shutdown();
    }

    bool is_open() const { return m_connection->is_open(); }

    void retrieve_http_cookie(int client_id, u64 request_id, RequestServer::RequestType request_type, u64 cookie_request_id = 0)
    {
        auto message = make<Messages::RequestServerControl::RetrievedHttpCookie>(client_id, request_id, request_type, cookie_request_id, String {});
        auto response = MUST(static_cast<RequestServerControlEndpoint::Stub&>(*m_connection).handle(move(message)));
        VERIFY(!response);
    }

    NonnullOwnPtr<Messages::RequestServerControlClient::RetrieveHttpCookie> take_cookie_request()
    {
        m_remote_transport->wait_until_readable();

        OwnPtr<Messages::RequestServerControlClient::RetrieveHttpCookie> cookie_request;
        auto should_shutdown = m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestServerControlClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
            VERIFY(message->message_id() == Messages::RequestServerControlClient::RetrieveHttpCookie::static_message_id());
            VERIFY(!cookie_request);
            cookie_request = message.template release_nonnull<Messages::RequestServerControlClient::RetrieveHttpCookie>();
        });
        VERIFY(should_shutdown == IPC::Transport::ShouldShutdown::No);
        VERIFY(cookie_request);
        return cookie_request.release_nonnull();
    }

private:
    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ControlConnectionFromClient> m_connection;
};

// A data connection, as handed out by the control connection to each helper process.
class TestConnection {
public:
    explicit TestConnection(TestServer& server)
    {
        initialize_libcurl();

        auto pair = MUST(IPC::Transport::create_paired());
        m_remote_transport = MUST(pair.remote_handle.create_transport());
        m_connection = RequestServer::ConnectionFromClient::construct(
            move(pair.local), RequestServer::IsPrivate::No,
            server.connections, server.request_transfer_leases, Optional<HTTP::DiskCache&> {}, ByteString {});
#ifdef AK_OS_WINDOWS
        auto pid = Core::System::getpid();
        m_connection->transport().set_peer_pid(pid);
        m_remote_transport->set_peer_pid(pid);
#endif
    }

    ~TestConnection()
    {
        if (m_connection->is_open())
            m_connection->shutdown();
    }

    int client_id() const { return m_connection->client_id(); }
    bool is_open() const { return m_connection->is_open(); }

    void stop_request(u64 request_id)
    {
        auto message = make<Messages::RequestServer::StopRequest>(request_id);
        auto response = dispatch(move(message));
        VERIFY(response);
    }

    void start_request(u64 request_id)
    {
        auto url = URL::Parser::basic_parse("http://localhost"sv).release_value();
        auto message = make<Messages::RequestServer::StartRequest>(request_id, ByteString { "GET" }, move(url), Vector<HTTP::Header> {}, ByteBuffer {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, true, Optional<u32> {}, false, 0, 0);
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

    void adopt_request(int source_client_id, u64 source_request_id, u64 target_request_id)
    {
        auto message = make<Messages::RequestServer::AdoptRequest>(source_client_id, source_request_id, target_request_id, false);
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

    void send_shared_websocket_message(size_t size)
    {
        auto data = MUST(Core::AnonymousBuffer::create_with_size(size));
        auto message = make<Messages::RequestServer::WebsocketSendShared>(0, false, move(data));
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

private:
    OwnPtr<IPC::MessageBuffer> dispatch(NonnullOwnPtr<IPC::Message> message)
    {
        return MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
    }

    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
};

}

TEST_CASE(cookie_responses_cannot_target_connect_requests)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection connection { server };

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Connect);

    EXPECT(!control.is_open());
}

TEST_CASE(stale_cookie_response_for_disconnected_client_is_ignored)
{
    TestServer server;
    TestControlConnection control { server };

    control.retrieve_http_cookie(-1, 0, RequestServer::RequestType::Fetch);

    EXPECT(control.is_open());
}

TEST_CASE(stale_cookie_response_for_cancelled_request_is_ignored)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection connection { server };
    connection.start_request(0);
    auto cookie_request = control.take_cookie_request();
    connection.stop_request(0);

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());

    EXPECT(control.is_open());
}

TEST_CASE(stale_cookie_response_cannot_target_replacement_request)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection connection { server };
    connection.start_request(0);
    auto stale_cookie_request = control.take_cookie_request();
    connection.stop_request(0);

    connection.start_request(0);
    auto current_cookie_request = control.take_cookie_request();
    EXPECT_NE(stale_cookie_request->cookie_request_id(), current_cookie_request->cookie_request_id());

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, stale_cookie_request->cookie_request_id());
    EXPECT(control.is_open());

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, current_cookie_request->cookie_request_id());
    EXPECT(control.is_open());
}

TEST_CASE(duplicate_cookie_response_is_rejected)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection connection { server };
    connection.start_request(0);
    auto cookie_request = control.take_cookie_request();

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());
    EXPECT(control.is_open());

    control.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());
    EXPECT(!control.is_open());
}

TEST_CASE(shared_websocket_messages_obey_the_payload_limit)
{
    TestServer server;
    TestConnection connection { server };

    connection.send_shared_websocket_message(IPC::MAX_MESSAGE_PAYLOAD_SIZE);
    EXPECT(connection.is_open());
    connection.send_shared_websocket_message(IPC::MAX_MESSAGE_PAYLOAD_SIZE + 1);
    EXPECT(!connection.is_open());
}

TEST_CASE(transferring_request_reissues_cookie_lookup_for_new_owner)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection source_connection { server };
    TestConnection target_connection { server };

    source_connection.start_request(0);
    auto initial_cookie_request = control.take_cookie_request();
    EXPECT_EQ(initial_cookie_request->client_id(), source_connection.client_id());
    EXPECT_EQ(initial_cookie_request->request_id(), 0u);

    target_connection.adopt_request(source_connection.client_id(), 0, 1);
    auto transferred_cookie_request = control.take_cookie_request();
    EXPECT_EQ(transferred_cookie_request->client_id(), target_connection.client_id());
    EXPECT_EQ(transferred_cookie_request->request_id(), 1u);

    EXPECT_NE(initial_cookie_request->cookie_request_id(), transferred_cookie_request->cookie_request_id());

    control.retrieve_http_cookie(source_connection.client_id(), 0, RequestServer::RequestType::Fetch, initial_cookie_request->cookie_request_id());
    EXPECT(control.is_open());
}
