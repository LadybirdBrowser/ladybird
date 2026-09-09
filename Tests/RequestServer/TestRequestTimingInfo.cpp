/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
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

// A local HTTP server that answers every request with the same small response.
class HTTPServer {
public:
    HTTPServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));

        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));

            auto connection_index = m_connections.size();
            m_connections.append(Connection { move(socket) });

            auto& connection = m_connections.last();
            connection.socket->on_ready_to_read = [this, connection_index] {
                on_ready_to_read(connection_index);
            };
        };
    }

    URL::URL url_for_path(StringView path) const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}{}", *m_server->local_port(), path)).release_value();
    }

private:
    struct Connection {
        explicit Connection(NonnullOwnPtr<Core::TCPSocket> socket)
            : socket(move(socket))
        {
        }

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer request;
    };

    void on_ready_to_read(size_t connection_index)
    {
        auto& connection = m_connections[connection_index];

        auto buffer = MUST(ByteBuffer::create_uninitialized(4096));
        auto bytes = MUST(connection.socket->read_some(buffer));
        if (bytes.is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }

        MUST(connection.request.try_append(bytes));
        if (!StringView { connection.request }.contains("\r\n\r\n"sv))
            return;

        connection.socket->on_ready_to_read = nullptr;
        MUST(connection.socket->set_blocking(true));

        auto body = "hello"sv;
        auto response = ByteString::formatted(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: {}\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n"
            "\r\n"
            "{}",
            body.length(), body);
        MUST(connection.socket->write_until_depleted(response.bytes()));
        connection.socket->close();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<Connection> m_connections;
};

struct TestServer {
    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
};

struct FinishedRequest {
    Requests::RequestTimingInfo timing_info;
    Optional<Requests::NetworkError> network_error;
    // Wall-clock time from starting the request until RequestServer reported it finished.
    AK::Duration elapsed;
};

class TestConnection {
public:
    explicit TestConnection(TestServer& server)
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
            move(pair.local), RequestServer::ConnectionFromClient::IsPrimaryConnection::Yes, RequestServer::IsPrivate::No,
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

    Optional<FinishedRequest> fetch(u64 request_id, URL::URL url, AK::Duration budget)
    {
        auto started_at = MonotonicTime::now();
        m_started_at.set(request_id, started_at);

        auto message = make<Messages::RequestServer::StartRequest>(request_id, ByteString { "GET" }, move(url), Vector<HTTP::Header> {}, ByteBuffer {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::No, false, Optional<u32> {}, false);
        auto response = MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
        VERIFY(!response);

        auto deadline = started_at + budget;
        while (MonotonicTime::now() < deadline) {
            m_server.event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            drain_client_messages();

            if (auto finished = m_finished_requests.get(request_id); finished.has_value())
                return *finished;
        }
        return {};
    }

private:
    void drain_client_messages()
    {
        (void)m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));

            if (message->message_id() == Messages::RequestClient::RequestStarted::static_message_id()) {
                auto& started = static_cast<Messages::RequestClient::RequestStarted&>(*message);
                auto response_socket = MUST(Core::LocalSocket::adopt_fd(started.fd().take_fd()));
                response_socket->set_notifications_enabled(false);
                m_response_sockets.set(started.request_id(), move(response_socket));
            }

            if (message->message_id() == Messages::RequestClient::RequestFinished::static_message_id()) {
                auto& finished = static_cast<Messages::RequestClient::RequestFinished&>(*message);
                m_finished_requests.set(finished.request_id(), FinishedRequest { finished.timing_info(), finished.network_error(), MonotonicTime::now() - m_started_at.get(finished.request_id()).value() });
            }
        });

        auto buffer = MUST(ByteBuffer::create_uninitialized(4 * KiB));
        for (auto& response_socket : m_response_sockets)
            (void)response_socket.value->read_some(buffer);
    }

    TestServer& m_server;
    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
    HashMap<u64, NonnullOwnPtr<Core::LocalSocket>> m_response_sockets;
    HashMap<u64, MonotonicTime> m_started_at;
    HashMap<u64, FinishedRequest> m_finished_requests;
};

}

// Every timing phase is an offset from the start of the transfer, so they must be ordered and none may exceed the
// wall-clock time the request took.
TEST_CASE(timing_phases_are_ordered_and_bounded_by_the_request_duration)
{
    TestServer server;
    TestConnection connection { server };
    HTTPServer http_server;

    auto finished = connection.fetch(1, http_server.url_for_path("/resource"sv), AK::Duration::from_seconds(10));
    EXPECT(finished.has_value());
    if (!finished.has_value())
        return;
    EXPECT(!finished->network_error.has_value());

    auto const& timing = finished->timing_info;
    EXPECT(timing.domain_lookup_start_microseconds >= 0);
    EXPECT(timing.domain_lookup_end_microseconds >= timing.domain_lookup_start_microseconds);
    EXPECT_EQ(timing.connect_start_microseconds, timing.domain_lookup_end_microseconds);
    EXPECT(timing.connect_end_microseconds >= timing.connect_start_microseconds);
    EXPECT(timing.request_start_microseconds >= timing.connect_end_microseconds);
    EXPECT(timing.response_start_microseconds >= timing.request_start_microseconds);
    EXPECT(timing.response_end_microseconds >= timing.response_start_microseconds);
    EXPECT(timing.response_end_microseconds > 0);
    EXPECT(timing.response_end_microseconds <= finished->elapsed.to_microseconds());

    EXPECT_EQ(timing.encoded_body_size, 5);
    EXPECT_EQ(timing.http_version_alpn_identifier, Requests::ALPNHttpVersion::Http1_1);
}

// Without a TLS handshake there is no secure connection start, rather than one coinciding with the TCP connection.
TEST_CASE(plain_http_has_no_secure_connection_start)
{
    TestServer server;
    TestConnection connection { server };
    HTTPServer http_server;

    auto finished = connection.fetch(1, http_server.url_for_path("/resource"sv), AK::Duration::from_seconds(10));
    EXPECT(finished.has_value());
    if (!finished.has_value())
        return;

    EXPECT_EQ(finished->timing_info.secure_connect_start_microseconds, 0);
}
