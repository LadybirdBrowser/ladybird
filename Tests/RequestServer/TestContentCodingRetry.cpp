/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

// A local HTTP server that records the head of each request, and answers each with a body whose content coding libcurl
// cannot decode, so that RequestServer fetches it again with decoding disabled.
class UnsupportedCodingServer {
public:
    UnsupportedCodingServer()
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));

        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            auto index = m_connections.size();
            m_connections.append({ move(socket), {} });
            m_connections.last().socket->on_ready_to_read = [this, index] { on_ready_to_read(index); };
        };
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/resource", *m_server->local_port())).release_value();
    }

    Vector<ByteString> const& request_heads() const { return m_request_heads; }

private:
    struct Connection {
        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer request;
    };

    void on_ready_to_read(size_t index)
    {
        auto& connection = m_connections[index];
        auto buffer = MUST(ByteBuffer::create_uninitialized(4096));
        auto bytes = MUST(connection.socket->read_some(buffer));
        if (bytes.is_empty()) {
            connection.socket->on_ready_to_read = nullptr;
            return;
        }

        MUST(connection.request.try_append(bytes));
        auto request = StringView { connection.request };
        auto head_end = request.find("\r\n\r\n"sv);
        if (!head_end.has_value())
            return;

        connection.socket->on_ready_to_read = nullptr;
        m_request_heads.append(request.substring_view(0, *head_end));

        MUST(connection.socket->set_blocking(true));
        MUST(connection.socket->write_until_depleted(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Encoding: base64\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\n"
            "aGk="sv.bytes()));
        connection.socket->close();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<Connection> m_connections;
    Vector<ByteString> m_request_heads;
};

}

// When RequestServer fetches a response again because libcurl cannot decode its content coding, it looks up the
// cookies for the retry again. A Cookie header that the client supplied itself is not part of that lookup, so the
// retried request must still carry it.
TEST_CASE(content_coding_retry_keeps_client_supplied_cookie_header)
{
    Core::EventLoop event_loop;
    MUST(RequestServer::initialize_libcurl());

    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    auto pair = MUST(IPC::Transport::create_paired());
    auto remote_transport = MUST(pair.remote_handle.create_transport());
    auto connection = RequestServer::ConnectionFromClient::construct(
        move(pair.local), RequestServer::IsPrivate::No, connections, request_transfer_leases, Optional<HTTP::DiskCache&> {}, ByteString {});
#ifdef AK_OS_WINDOWS
    auto pid = Core::System::getpid();
    connection->transport().set_peer_pid(pid);
    remote_transport->set_peer_pid(pid);
#endif

    UnsupportedCodingServer server;
    auto message = make<Messages::RequestServer::StartRequest>(
        0, ByteString { "GET" }, server.url(), Vector<HTTP::Header> { { "Cookie", "supplied=by-the-client" } }, ByteBuffer {},
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No, false, Optional<u32> {}, false, 0, 0);
    VERIFY(!MUST(static_cast<RequestServerEndpoint::Stub&>(*connection).handle(move(message))));

    event_loop.spin_until([&] { return server.request_heads().size() >= 2; });

    for (auto const& head : server.request_heads())
        EXPECT(head.contains("Cookie: supplied=by-the-client"sv));

    connection->shutdown();
}
