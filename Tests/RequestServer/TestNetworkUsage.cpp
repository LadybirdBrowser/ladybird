/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebSocket/WebSocket.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/ControlConnectionFromClient.h>

namespace {

struct TestServer {
    TestServer()
    {
        static bool libcurl_initialized = [] {
            MUST(RequestServer::initialize_libcurl());
            return true;
        }();
        (void)libcurl_initialized;

        auto control_pair = MUST(IPC::Transport::create_paired());
        control_remote_transport = MUST(control_pair.remote_handle.create_transport());
        control_connection = RequestServer::ControlConnectionFromClient::construct(
            move(control_pair.local), connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

        auto pair = MUST(IPC::Transport::create_paired());
        remote_transport = MUST(pair.remote_handle.create_transport());
        connection = RequestServer::ConnectionFromClient::construct(
            move(pair.local), RequestServer::IsPrivate::No, RequestServer::SiteBinding::Unrestricted,
            connections, request_transfer_leases, Optional<HTTP::DiskCache&> {});

        auto message = make<Messages::RequestServerControl::SetPerformanceMonitorEnabled>(true);
        VERIFY(!MUST(static_cast<RequestServerControlEndpoint::Stub&>(*control_connection).handle(move(message))));
    }

    ~TestServer()
    {
        if (connection->is_open())
            connection->shutdown();
        if (control_connection->is_open())
            control_connection->shutdown();
    }

    void start_request(u64 request_id, URL::URL url, u64 originating_page_id)
    {
        auto message = make<Messages::RequestServer::StartRequest>(
            request_id, ByteString { "GET" }, move(url), Vector<HTTP::Header> {}, ByteBuffer {},
            HTTP::CacheMode::NoStore, Optional<HTTP::NetworkIsolationKey> {}, HTTP::Cookie::IncludeCredentials::No, false, Optional<u32> {}, false, originating_page_id, HTTP::Cookie::SameSiteContext {});
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    }

    void websocket_connect(u64 websocket_id, URL::URL url, u64 originating_page_id)
    {
        auto message = make<Messages::RequestServer::WebsocketConnect>(
            websocket_id, move(url), Optional<HTTP::NetworkIsolationKey> {}, ByteString { "http://127.0.0.1" }, Vector<ByteString> {},
            Vector<ByteString> {}, Vector<HTTP::Header> {}, originating_page_id, HTTP::Cookie::SameSiteContext {});
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    }

    void websocket_send(u64 websocket_id, StringView text)
    {
        auto message = make<Messages::RequestServer::WebsocketSend>(websocket_id, true, MUST(ByteBuffer::copy(text.bytes())));
        VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));
    }

    // Runs the event loop until RequestServer tells the client that the WebSocket is open.
    void wait_for_websocket_connected(u64 websocket_id)
    {
        // The transports are not part of the event loop here, so keep the loop from sleeping until they have news.
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        bool connected = false;
        event_loop.spin_until([&] {
            read_control_messages();
            (void)remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
                auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
                if (message->message_id() != Messages::RequestClient::WebsocketConnected::static_message_id())
                    return;
                if (static_cast<Messages::RequestClient::WebsocketConnected&>(*message).websocket_id() == websocket_id)
                    connected = true;
            });
            return connected;
        });
    }

    // Runs the event loop until RequestServer has reported at least this much traffic, and returns its reports.
    Vector<Requests::NetworkUsage> wait_for_network_usage(u64 download_bytes, u64 upload_bytes)
    {
        auto wake_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(10), [] { });
        wake_timer->start();

        event_loop.spin_until([&] {
            read_control_messages();
            u64 downloaded = 0;
            u64 uploaded = 0;
            for (auto const& entry : network_usage) {
                downloaded += entry.download_bytes;
                uploaded += entry.upload_bytes;
            }
            return downloaded >= download_bytes && uploaded >= upload_bytes;
        });
        return move(network_usage);
    }

    // Keeps RequestServer's reports of network usage, and answers its cookie lookups with no cookies.
    void read_control_messages()
    {
        Vector<NonnullOwnPtr<Messages::RequestServerControlClient::RetrieveHttpCookie>> cookie_requests;
        (void)control_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestServerControlClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
            if (message->message_id() == Messages::RequestServerControlClient::RetrieveHttpCookie::static_message_id()) {
                cookie_requests.append(message.template release_nonnull<Messages::RequestServerControlClient::RetrieveHttpCookie>());
                return;
            }
            if (message->message_id() == Messages::RequestServerControlClient::NetworkUsage::static_message_id())
                network_usage.extend(static_cast<Messages::RequestServerControlClient::NetworkUsage&>(*message).take_usage());
        });

        for (auto const& cookie_request : cookie_requests) {
            auto message = make<Messages::RequestServerControl::RetrievedHttpCookie>(
                cookie_request->client_id(), cookie_request->request_id(), cookie_request->request_type(), cookie_request->cookie_request_id(), String {});
            VERIFY(!MUST(static_cast<RequestServerControlEndpoint::Stub&>(*control_connection).handle(move(message))));
        }
    }

    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    OwnPtr<IPC::Transport> control_remote_transport;
    RefPtr<RequestServer::ControlConnectionFromClient> control_connection;
    OwnPtr<IPC::Transport> remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> connection;
    Vector<Requests::NetworkUsage> network_usage;
};

// A local HTTP server that answers every request with a body of the given size.
class HTTPServer {
public:
    explicit HTTPServer(size_t body_size)
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this, body_size] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            m_sockets.append(move(socket));
            auto& connection = *m_sockets.last();
            connection.on_ready_to_read = [&connection, body_size] {
                auto buffer = MUST(ByteBuffer::create_uninitialized(4096));
                if (MUST(connection.read_some(buffer)).is_empty())
                    return;
                connection.on_ready_to_read = nullptr;
                MUST(connection.set_blocking(true));
                auto response = ByteString::formatted("HTTP/1.1 200 OK\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body_size, ByteString::repeated('x', body_size));
                MUST(connection.write_until_depleted(response.bytes()));
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

// A local WebSocket server that greets every client with a text message.
class WebSocketServer {
public:
    explicit WebSocketServer(StringView greeting)
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this, greeting = ByteString { greeting }] {
            auto websocket = MUST(WebSocket::WebSocket::accept(MUST(m_server->accept()), {}));
            websocket->on_open = [&websocket = *websocket, greeting] {
                websocket.send(WebSocket::Message { greeting });
            };
            websocket->start();
            m_websockets.append(move(websocket));
        };
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("ws://127.0.0.1:{}/", *m_server->local_port())).release_value();
    }

private:
    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullRefPtr<WebSocket::WebSocket>> m_websockets;
};

}

TEST_CASE(request_traffic_is_reported_under_the_client_that_made_it)
{
    TestServer server;
    HTTPServer http_server(1000);

    server.start_request(1, http_server.url(), 42);

    auto usage = server.wait_for_network_usage(1000, 0);
    u64 downloaded = 0;
    for (auto const& entry : usage) {
        EXPECT_EQ(entry.client_id, server.connection->client_id());
        EXPECT_EQ(entry.page_id, 42u);
        downloaded += entry.download_bytes;
    }
    EXPECT_EQ(downloaded, 1000u);
}

TEST_CASE(websocket_traffic_is_reported_as_it_crosses_the_socket)
{
    TestServer server;
    WebSocketServer websocket_server("hello"sv);

    server.websocket_connect(1, websocket_server.url(), 42);
    server.wait_for_websocket_connected(1);
    server.websocket_send(1, "hi there"sv);

    // The server's frame has a 2-byte header, and ours a 2-byte header and a 4-byte mask.
    auto usage = server.wait_for_network_usage(2 + 5, 6 + 8);
    u64 downloaded = 0;
    u64 uploaded = 0;
    for (auto const& entry : usage) {
        EXPECT_EQ(entry.client_id, server.connection->client_id());
        EXPECT_EQ(entry.page_id, 42u);
        downloaded += entry.download_bytes;
        uploaded += entry.upload_bytes;
    }
    EXPECT_EQ(downloaded, 2u + 5u);
    EXPECT_EQ(uploaded, 6u + 8u);
}
