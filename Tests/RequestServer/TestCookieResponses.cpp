/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
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

class TestConnection {
public:
    TestConnection(TestServer& server, RequestServer::ConnectionFromClient::IsPrimaryConnection is_primary_connection)
        : m_server(server)
    {
        static bool libcurl_initialized = [] {
            MUST(RequestServer::initialize_libcurl());
            return true;
        }();
        (void)libcurl_initialized;

        auto pair = MUST(IPC::Transport::create_paired());
        m_remote_transport = MUST(pair.remote_handle.create_transport());
        m_connection = RequestServer::ConnectionFromClient::construct(
            move(pair.local), is_primary_connection, RequestServer::IsPrivate::No,
            m_server.connections, m_server.request_transfer_leases, Optional<HTTP::DiskCache&> {}, ByteString {});
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

    void retrieve_http_cookie(int client_id, u64 request_id, RequestServer::RequestType request_type, u64 cookie_request_id = 0)
    {
        auto message = make<Messages::RequestServer::RetrievedHttpCookie>(client_id, request_id, request_type, cookie_request_id, String {});
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

    void stop_request(u64 request_id)
    {
        auto message = make<Messages::RequestServer::StopRequest>(request_id);
        auto response = dispatch(move(message));
        VERIFY(response);
    }

    void start_request(u64 request_id)
    {
        auto url = URL::Parser::basic_parse("http://localhost"sv).release_value();
        auto message = make<Messages::RequestServer::StartRequest>(request_id, ByteString { "GET" }, move(url), Vector<HTTP::Header> {}, ByteBuffer {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::Yes, Core::ProxyData {}, true, Optional<u32> {});
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

    void adopt_request(int source_client_id, u64 source_request_id, u64 target_request_id)
    {
        auto message = make<Messages::RequestServer::AdoptRequest>(source_client_id, source_request_id, target_request_id, false);
        auto response = dispatch(move(message));
        VERIFY(!response);
    }

    NonnullOwnPtr<Messages::RequestClient::RetrieveHttpCookie> take_cookie_request()
    {
        m_remote_transport->wait_until_readable();

        OwnPtr<Messages::RequestClient::RetrieveHttpCookie> cookie_request;
        auto should_shutdown = m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
            VERIFY(message->message_id() == Messages::RequestClient::RetrieveHttpCookie::static_message_id());
            VERIFY(!cookie_request);
            cookie_request = message.template release_nonnull<Messages::RequestClient::RetrieveHttpCookie>();
        });
        VERIFY(should_shutdown == IPC::Transport::ShouldShutdown::No);
        VERIFY(cookie_request);
        return cookie_request.release_nonnull();
    }

private:
    OwnPtr<IPC::MessageBuffer> dispatch(NonnullOwnPtr<IPC::Message> message)
    {
        return MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
    }

    TestServer& m_server;
    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
};

}

TEST_CASE(non_primary_connection_cannot_send_cookie_responses)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::No };

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch);

    EXPECT(!connection.is_open());
}

TEST_CASE(cookie_responses_cannot_target_connect_requests)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Connect);

    EXPECT(!connection.is_open());
}

TEST_CASE(stale_cookie_response_for_disconnected_client_is_ignored)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };

    connection.retrieve_http_cookie(-1, 0, RequestServer::RequestType::Fetch);

    EXPECT(connection.is_open());
}

TEST_CASE(stale_cookie_response_for_cancelled_request_is_ignored)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };
    connection.start_request(0);
    auto cookie_request = connection.take_cookie_request();
    connection.stop_request(0);

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());

    EXPECT(connection.is_open());
}

TEST_CASE(stale_cookie_response_cannot_target_replacement_request)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };
    connection.start_request(0);
    auto stale_cookie_request = connection.take_cookie_request();
    connection.stop_request(0);

    connection.start_request(0);
    auto current_cookie_request = connection.take_cookie_request();
    EXPECT_NE(stale_cookie_request->cookie_request_id(), current_cookie_request->cookie_request_id());

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, stale_cookie_request->cookie_request_id());
    EXPECT(connection.is_open());

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, current_cookie_request->cookie_request_id());
    EXPECT(connection.is_open());
}

TEST_CASE(duplicate_cookie_response_is_rejected)
{
    TestServer server;
    TestConnection connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };
    connection.start_request(0);
    auto cookie_request = connection.take_cookie_request();

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());
    EXPECT(connection.is_open());

    connection.retrieve_http_cookie(connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());
    EXPECT(!connection.is_open());
}

TEST_CASE(transferring_request_reissues_cookie_lookup_for_new_owner)
{
    TestServer server;
    TestConnection primary_connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes };
    TestConnection source_connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::No };
    TestConnection target_connection { server, RequestServer::ConnectionFromClient::IsPrimaryConnection::No };

    source_connection.start_request(0);
    auto initial_cookie_request = primary_connection.take_cookie_request();
    EXPECT_EQ(initial_cookie_request->client_id(), source_connection.client_id());
    EXPECT_EQ(initial_cookie_request->request_id(), 0u);

    target_connection.adopt_request(source_connection.client_id(), 0, 1);
    auto transferred_cookie_request = primary_connection.take_cookie_request();
    EXPECT_EQ(transferred_cookie_request->client_id(), target_connection.client_id());
    EXPECT_EQ(transferred_cookie_request->request_id(), 1u);

    EXPECT_NE(initial_cookie_request->cookie_request_id(), transferred_cookie_request->cookie_request_id());

    primary_connection.retrieve_http_cookie(source_connection.client_id(), 0, RequestServer::RequestType::Fetch, initial_cookie_request->cookie_request_id());
    EXPECT(primary_connection.is_open());
}
