/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibIPC/Transport.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

namespace {

// A local HTTP server that answers some connections and leaves others hanging forever with the request unanswered,
// which is what a network path that silently died looks like to RequestServer.
class StallingServer {
public:
    using ResponseForConnection = Function<Optional<ByteString>(size_t connection_index)>;

    explicit StallingServer(ResponseForConnection response_for_connection)
        : m_response_for_connection(move(response_for_connection))
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));

        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));

            auto connection_index = m_connections.size();
            m_connections.append(Connection { move(socket), {} });

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

    size_t connection_count() const { return m_connections.size(); }

private:
    struct Connection {
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

        // The request head is complete. Either answer it now, or never.
        connection.socket->on_ready_to_read = nullptr;

        if (auto response = m_response_for_connection(connection_index); response.has_value()) {
            MUST(connection.socket->set_blocking(true));
            MUST(connection.socket->write_until_depleted(response->bytes()));
            connection.socket->close();
        }
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<Connection> m_connections;
    ResponseForConnection m_response_for_connection;
};

ByteString http_response(StringView cache_control, StringView body)
{
    return ByteString::formatted(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: {}\r\n"
        "Cache-Control: {}\r\n"
        "ETag: \"v1\"\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{}",
        body.length(), cache_control, body);
}

struct TestServer {
    Core::EventLoop event_loop;
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
};

struct FinishedRequest {
    Optional<Requests::NetworkError> network_error;
    // The disk cache's test mode reports how it treated the response: not-cached, written-to-cache or read-from-cache.
    ByteString cache_status;
};

class TestConnection {
public:
    TestConnection(TestServer& server, HTTP::DiskCache& disk_cache)
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
            m_server.connections, m_server.request_transfer_leases, disk_cache, ByteString {});
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

    void start_request(u64 request_id, URL::URL url)
    {
        // The disk cache only stores responses to requests that opt in while it runs in test mode.
        Vector<HTTP::Header> request_headers {
            { ByteString { HTTP::TEST_CACHE_ENABLED_HEADER }, "1"sv },
        };

        auto message = make<Messages::RequestServer::StartRequest>(request_id, ByteString { "GET" }, move(url), move(request_headers), ByteBuffer {}, HTTP::CacheMode::Default, HTTP::Cookie::IncludeCredentials::No, Core::ProxyData {}, false, Optional<u32> {});
        auto response = MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
        VERIFY(!response);
    }

    // Pumps the event loop until the request finishes or the budget runs out.
    Optional<FinishedRequest> wait_for_request_to_finish(u64 request_id, AK::Duration budget)
    {
        auto deadline = MonotonicTime::now() + budget;

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

            if (message->message_id() == Messages::RequestClient::HeadersBecameAvailable::static_message_id()) {
                auto& headers = static_cast<Messages::RequestClient::HeadersBecameAvailable&>(*message);
                for (auto const& header : headers.response_headers()) {
                    if (header.name.equals_ignoring_ascii_case(HTTP::TEST_CACHE_STATUS_HEADER))
                        m_cache_statuses.set(headers.request_id(), header.value);
                }
            }

            if (message->message_id() == Messages::RequestClient::RequestFinished::static_message_id()) {
                auto& finished = static_cast<Messages::RequestClient::RequestFinished&>(*message);
                m_finished_requests.set(finished.request_id(), FinishedRequest { finished.network_error(), m_cache_statuses.get(finished.request_id()).value_or({}) });
            }
        });
    }

    TestServer& m_server;
    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
    HashMap<u64, ByteString> m_cache_statuses;
    HashMap<u64, FinishedRequest> m_finished_requests;
};

HTTP::DiskCache create_test_disk_cache()
{
    auto cache_root = LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv);
    return MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, cache_root)).release_value();
}

void expect_finished_without_error(Optional<FinishedRequest> const& finished, StringView expected_cache_status)
{
    EXPECT(finished.has_value());
    if (!finished.has_value())
        return;

    if (finished->network_error.has_value())
        FAIL(ByteString::formatted("Request finished with network error {}", to_underlying(*finished->network_error)));
    EXPECT_EQ(finished->cache_status, expected_cache_status);
}

}

// A request whose cache entry is held open by a request that never completes must not wait forever: it goes to
// the network without the cache once the wait times out.
TEST_CASE(request_waiting_on_a_stalled_cache_entry_writer_eventually_bypasses_the_cache)
{
    RequestServer::Request::set_wait_for_cache_timeout(AK::Duration::from_milliseconds(250));
    RequestServer::Request::set_revalidation_stall_timeout(AK::Duration::from_seconds(60));

    TestServer server;
    auto disk_cache = create_test_disk_cache();
    TestConnection connection { server, disk_cache };

    // The first connection is never answered, so the first request keeps its cache entry writer open forever.
    StallingServer http_server { [](size_t connection_index) -> Optional<ByteString> {
        if (connection_index == 0)
            return {};
        return http_response("max-age=60"sv, "hello"sv);
    } };

    auto url = http_server.url_for_path("/resource"sv);
    connection.start_request(1, url);
    connection.start_request(2, url);

    auto finished = connection.wait_for_request_to_finish(2, AK::Duration::from_seconds(10));
    expect_finished_without_error(finished, "not-cached"sv);
    EXPECT_EQ(http_server.connection_count(), 2u);
}

// A background revalidation that stops receiving data is abandoned, which closes the cache entry it holds open and
// lets requests waiting on that entry proceed.
TEST_CASE(stalled_background_revalidation_is_abandoned_and_releases_waiting_requests)
{
    RequestServer::Request::set_wait_for_cache_timeout(AK::Duration::from_seconds(60));
    RequestServer::Request::set_revalidation_stall_timeout(AK::Duration::from_seconds(1));

    TestServer server;
    auto disk_cache = create_test_disk_cache();
    TestConnection connection { server, disk_cache };

    // The first connection stores a response that is stale immediately but may be served while it is revalidated
    // in the background. The second connection, the revalidation, is never answered.
    StallingServer http_server { [](size_t connection_index) -> Optional<ByteString> {
        if (connection_index == 1)
            return {};
        return http_response("max-age=0, stale-while-revalidate=600"sv, "hello"sv);
    } };

    auto url = http_server.url_for_path("/resource"sv);
    connection.start_request(1, url);
    expect_finished_without_error(connection.wait_for_request_to_finish(1, AK::Duration::from_seconds(10)), "written-to-cache"sv);

    // Served from the cache, and kicks off the background revalidation that will stall.
    connection.start_request(2, url);
    expect_finished_without_error(connection.wait_for_request_to_finish(2, AK::Duration::from_seconds(10)), "read-from-cache"sv);

    // Waits on the entry held by the stalled revalidation. Abandoning a revalidation discards the entry, so once that
    // happens this request fetches the resource anew, long before the 60 second wait limit could have let it go.
    connection.start_request(3, url);
    expect_finished_without_error(connection.wait_for_request_to_finish(3, AK::Duration::from_seconds(10)), "written-to-cache"sv);
    EXPECT_EQ(http_server.connection_count(), 3u);
}
