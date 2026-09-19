/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCache.h>
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

    void stored_response_cookies_and_hsts_policy(int client_id, u64 request_id, u64 store_request_id)
    {
        auto message = make<Messages::RequestServerControl::StoredResponseCookiesAndHstsPolicy>(client_id, request_id, store_request_id);
        auto response = MUST(static_cast<RequestServerControlEndpoint::Stub&>(*m_connection).handle(move(message)));
        VERIFY(!response);
    }

    // Runs the event loop until RequestServer asks the UI process to store a response's cookies and HSTS policy.
    NonnullOwnPtr<Messages::RequestServerControlClient::StoreResponseCookiesAndHstsPolicy> wait_for_storage_request(Core::EventLoop& event_loop)
    {
        // The control transport is not part of the event loop here, so keep the loop from sleeping until it has news.
        auto wake_timer = Core::Timer::create_repeating(10, [] { });
        wake_timer->start();

        OwnPtr<Messages::RequestServerControlClient::StoreResponseCookiesAndHstsPolicy> storage_request;
        event_loop.spin_until([&] {
            (void)m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestServerControlClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (!storage_request && message->message_id() == Messages::RequestServerControlClient::StoreResponseCookiesAndHstsPolicy::static_message_id())
                    storage_request = message.template release_nonnull<Messages::RequestServerControlClient::StoreResponseCookiesAndHstsPolicy>();
            });
            return storage_request != nullptr;
        });
        return storage_request.release_nonnull();
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

    void set_certificate(u64 request_id)
    {
        auto message = make<Messages::RequestServer::SetCertificate>(request_id, ByteString { "certificate" }, ByteString { "key" });
        auto response = dispatch(move(message));
        VERIFY(response);
    }

    void stop_request(u64 request_id)
    {
        auto message = make<Messages::RequestServer::StopRequest>(request_id);
        auto response = dispatch(move(message));
        VERIFY(response);
    }

    void start_request(u64 request_id, Optional<URL::URL> target_url = {})
    {
        auto url = target_url.value_or(URL::Parser::basic_parse("http://localhost"sv).release_value());
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

private:
    OwnPtr<IPC::MessageBuffer> dispatch(NonnullOwnPtr<IPC::Message> message)
    {
        return MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
    }

    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
};

}

namespace {

// A local HTTP server that answers every request with a response that sets a cookie.
class SetCookieServer {
public:
    SetCookieServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_sockets.append(move(socket));
            auto& connection = *m_sockets.last();
            connection.on_ready_to_read = [&connection] {
                auto buffer = MUST(ByteBuffer::create_uninitialized(4096));
                if (MUST(connection.read_some(buffer)).is_empty())
                    return;
                connection.on_ready_to_read = nullptr;
                MUST(connection.set_blocking(true));
                MUST(connection.write_until_depleted(
                    "HTTP/1.1 200 OK\r\nSet-Cookie: session=fresh\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"sv.bytes()));
            };
        };
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/", *m_server->local_port())).release_value();
    }

private:
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Core::TCPSocket>> m_sockets;
};

}

TEST_CASE(unsolicited_certificate_is_rejected)
{
    TestServer server;
    TestConnection connection { server };

    connection.set_certificate(0xc3c4c5c6c7c8c9ca);

    EXPECT(connection.is_open());
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

// A request waits for the UI process to store its response's cookies before its client sees the response. If the request
// moves to another client meanwhile, the acknowledgement for its old IDs can no longer reach it, so RequestServer asks
// again on behalf of the new owner.
TEST_CASE(transferring_request_reissues_response_storage_for_new_owner)
{
    TestServer server;
    TestControlConnection control { server };
    TestConnection source_connection { server };
    TestConnection target_connection { server };
    SetCookieServer http_server;

    source_connection.start_request(0, http_server.url());
    auto cookie_request = control.take_cookie_request();
    control.retrieve_http_cookie(source_connection.client_id(), 0, RequestServer::RequestType::Fetch, cookie_request->cookie_request_id());

    auto initial_storage_request = control.wait_for_storage_request(server.event_loop);
    EXPECT_EQ(initial_storage_request->client_id(), source_connection.client_id());
    EXPECT_EQ(initial_storage_request->request_id(), 0u);
    EXPECT_EQ(initial_storage_request->cookies().size(), 1u);

    target_connection.adopt_request(source_connection.client_id(), 0, 1);
    auto transferred_storage_request = control.wait_for_storage_request(server.event_loop);
    EXPECT_EQ(transferred_storage_request->client_id(), target_connection.client_id());
    EXPECT_EQ(transferred_storage_request->request_id(), 1u);
    EXPECT_EQ(transferred_storage_request->cookies().size(), 1u);
    EXPECT_NE(initial_storage_request->store_request_id(), transferred_storage_request->store_request_id());

    // The acknowledgement for the old IDs names a request that is gone, and is ignored. The one for the new owner lets
    // the response through.
    control.stored_response_cookies_and_hsts_policy(source_connection.client_id(), 0, initial_storage_request->store_request_id());
    control.stored_response_cookies_and_hsts_policy(target_connection.client_id(), 1, transferred_storage_request->store_request_id());
    EXPECT(control.is_open());
}
