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
#include <LibCore/Timer.h>
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

// How the local HTTP server answers one connection: Pieces go out one at a time, one per interval. Then, the connection
// is closed or else left open and silent forever (what a transfer whose network path died looks like to RequestServer).
struct ServerResponse {
    enum class CloseAfterLastPiece {
        No,
        Yes,
    };

    static ServerResponse at_once(ByteString response)
    {
        return { { move(response) }, AK::Duration {}, CloseAfterLastPiece::Yes };
    }

    Vector<ByteString> pieces;
    AK::Duration interval;
    CloseAfterLastPiece close_after_last_piece { CloseAfterLastPiece::Yes };
};

// A local HTTP server that answers some connections — either all at once, or else a piece at a time — and leaves others
// hanging forever, with the request unanswered.
class StallingServer {
public:
    using ResponseForConnection = Function<Optional<ServerResponse>(size_t connection_index)>;

    explicit StallingServer(ResponseForConnection response_for_connection)
        : m_response_for_connection(move(response_for_connection))
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

    size_t connection_count() const { return m_connections.size(); }

    // How many connections the server had seen by the time it wrote this connection's last piece — so a test can tell
    // whether a request that was waiting on this transfer held off until it finished.
    Optional<size_t> connection_count_when_finished(size_t connection_index) const { return m_connections[connection_index].connection_count_when_finished; }

private:
    struct Connection {
        explicit Connection(NonnullOwnPtr<Core::TCPSocket> socket)
            : socket(move(socket))
        {
        }

        NonnullOwnPtr<Core::TCPSocket> socket;
        ByteBuffer request;
        Optional<ServerResponse> response;
        size_t next_piece_index { 0 };
        RefPtr<Core::Timer> piece_timer;
        Optional<size_t> connection_count_when_finished;
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

        // The request head is complete. Either start answering it now, or never.
        connection.socket->on_ready_to_read = nullptr;

        auto response = m_response_for_connection(connection_index);
        if (!response.has_value())
            return;

        MUST(connection.socket->set_blocking(true));
        connection.response = response.release_value();
        write_next_piece(connection_index);
    }

    void write_next_piece(size_t connection_index)
    {
        auto& connection = m_connections[connection_index];
        auto const& response = *connection.response;

        MUST(connection.socket->write_until_depleted(response.pieces[connection.next_piece_index++].bytes()));

        if (connection.next_piece_index < response.pieces.size()) {
            connection.piece_timer = Core::Timer::create_single_shot(static_cast<int>(response.interval.to_milliseconds()), [this, connection_index] {
                write_next_piece(connection_index);
            });
            connection.piece_timer->start();
            return;
        }

        connection.connection_count_when_finished = m_connections.size();
        if (response.close_after_last_piece == ServerResponse::CloseAfterLastPiece::Yes)
            connection.socket->close();
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<Connection> m_connections;
    ResponseForConnection m_response_for_connection;
};

ByteString http_response_head(StringView cache_control, size_t body_length)
{
    return ByteString::formatted(
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: {}\r\n"
        "Cache-Control: {}\r\n"
        "ETag: \"v1\"\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_length, cache_control);
}

ByteString http_response(StringView cache_control, StringView body)
{
    return ByteString::formatted("{}{}", http_response_head(cache_control, body.length()), body);
}

// A response whose body goes out one byte per interval — so the request receiving it keeps making progress for as long
// as the bytes keep coming. Delivering fewer bytes than the head has promised leaves the connection open and silent
// after the last one: A transfer that has stalled for good.
ServerResponse trickled_http_response(StringView cache_control, StringView body, size_t bytes_to_deliver, AK::Duration interval)
{
    VERIFY(bytes_to_deliver <= body.length());

    ServerResponse response;
    response.pieces.append(http_response_head(cache_control, body.length()));
    for (size_t i = 0; i < bytes_to_deliver; ++i)
        response.pieces.append(ByteString { body.substring_view(i, 1) });

    response.interval = interval;
    response.close_after_last_piece = bytes_to_deliver == body.length()
        ? ServerResponse::CloseAfterLastPiece::Yes
        : ServerResponse::CloseAfterLastPiece::No;
    return response;
}

// A response whose body goes out a piece at a time — so the whole of it reaches curl quickly, without the server ever
// blocking the event loop on one write.
ServerResponse chunked_http_response(StringView cache_control, StringView body, size_t piece_size, AK::Duration interval)
{
    ServerResponse response;
    response.pieces.append(http_response_head(cache_control, body.length()));
    for (size_t offset = 0; offset < body.length(); offset += piece_size)
        response.pieces.append(ByteString { body.substring_view(offset, min(piece_size, body.length() - offset)) });

    response.interval = interval;
    return response;
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

    // Pumps the event loop until the request finishes or the budget runs out — reading every response as it comes.
    Optional<FinishedRequest> wait_for_request_to_finish(u64 request_id, AK::Duration budget)
    {
        return wait_for_request_to_finish_reading_slowly(request_id, budget, 64 * KiB, AK::Duration {});
    }

    // Pumps the event loop until the request finishes or the budget runs out — reading a bounded amount of every
    // response, no more often than every interval: A client that's slow to consume what it asked for, but never stops.
    // Reading less than RequestServer has ready leaves it holding the rest.
    Optional<FinishedRequest> wait_for_request_to_finish_reading_slowly(u64 request_id, AK::Duration budget, size_t bytes_per_read, AK::Duration interval)
    {
        auto deadline = MonotonicTime::now() + budget;
        auto next_read = MonotonicTime::now();

        while (MonotonicTime::now() < deadline) {
            m_server.event_loop.pump(Core::EventLoop::WaitMode::PollForEvents);
            drain_client_messages();

            if (MonotonicTime::now() >= next_read) {
                read_from_response_sockets(bytes_per_read);
                next_read = MonotonicTime::now() + interval;
            }

            if (auto finished = m_finished_requests.get(request_id); finished.has_value())
                return *finished;
        }

        return {};
    }

private:
    void read_from_response_sockets(size_t max_bytes_per_socket)
    {
        if (m_response_read_buffer.size() < max_bytes_per_socket)
            m_response_read_buffer = MUST(ByteBuffer::create_uninitialized(max_bytes_per_socket));

        for (auto& response_socket : m_response_sockets)
            (void)response_socket.value->read_some(m_response_read_buffer.span().slice(0, max_bytes_per_socket));
    }

    void drain_client_messages()
    {
        (void)m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));

            if (message->message_id() == Messages::RequestClient::RequestStarted::static_message_id()) {
                auto& started = static_cast<Messages::RequestClient::RequestStarted&>(*message);

                // Take the read end off the message, so it outlives it: A client that lets the pipe close breaks the
                // transfer, and one that never reads it wedges a response too big for the pipe to hold. The tests read
                // it at a pace of their own — so, its notifier stays off.
                auto response_socket = MUST(Core::LocalSocket::adopt_fd(started.fd().take_fd()));
                response_socket->set_notifications_enabled(false);
                m_response_sockets.set(started.request_id(), move(response_socket));
            }

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
    HashMap<u64, NonnullOwnPtr<Core::LocalSocket>> m_response_sockets;
    ByteBuffer m_response_read_buffer;
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
    StallingServer http_server { [](size_t connection_index) -> Optional<ServerResponse> {
        if (connection_index == 0)
            return {};
        return ServerResponse::at_once(http_response("max-age=60"sv, "hello"sv));
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
    StallingServer http_server { [](size_t connection_index) -> Optional<ServerResponse> {
        if (connection_index == 1)
            return {};
        return ServerResponse::at_once(http_response("max-age=0, stale-while-revalidate=600"sv, "hello"sv));
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

// A request whose cache entry is held open by a request still receiving data (however slowly) keeps waiting — rather
// than fetching a second copy over the network. And it reads the entry after that request has finished filling it.
TEST_CASE(request_waiting_on_a_slow_cache_entry_writer_keeps_waiting_and_reads_the_finished_entry)
{
    RequestServer::Request::set_wait_for_cache_timeout(AK::Duration::from_seconds(1));
    RequestServer::Request::set_revalidation_stall_timeout(AK::Duration::from_seconds(60));

    TestServer server;
    auto disk_cache = create_test_disk_cache();
    TestConnection connection { server, disk_cache };

    // The body arrives one byte every 100ms. So, the transfer takes a whole wait limit — without ever going quiet for
    // more than a tenth of one.
    StallingServer http_server { [](size_t) -> Optional<ServerResponse> {
        return trickled_http_response("max-age=60"sv, "0123456789"sv, 10, AK::Duration::from_milliseconds(100));
    } };

    auto url = http_server.url_for_path("/resource"sv);
    connection.start_request(1, url);
    connection.start_request(2, url);

    expect_finished_without_error(connection.wait_for_request_to_finish(1, AK::Duration::from_seconds(10)), "written-to-cache"sv);
    expect_finished_without_error(connection.wait_for_request_to_finish(2, AK::Duration::from_seconds(10)), "read-from-cache"sv);
    EXPECT_EQ(http_server.connection_count(), 1u);
}

// A request whose cache entry is held open by a request that received data for a while, and then stopped for good,
// bypasses the cache once that request has been quiet for a whole wait limit — and not before.
TEST_CASE(request_waiting_on_a_cache_entry_writer_that_stopped_receiving_data_bypasses_the_cache_once_it_stalls)
{
    RequestServer::Request::set_wait_for_cache_timeout(AK::Duration::from_seconds(1));
    RequestServer::Request::set_revalidation_stall_timeout(AK::Duration::from_seconds(60));

    TestServer server;
    auto disk_cache = create_test_disk_cache();
    TestConnection connection { server, disk_cache };

    // The first connection delivers half of its body, one byte every 100ms, and then falls silent — with the connection
    // left open. Any later connection is answered at once.
    StallingServer http_server { [](size_t connection_index) -> Optional<ServerResponse> {
        if (connection_index == 0)
            return trickled_http_response("max-age=60"sv, "0123456789abcdefghij"sv, 10, AK::Duration::from_milliseconds(100));
        return ServerResponse::at_once(http_response("max-age=60"sv, "hello"sv));
    } };

    auto url = http_server.url_for_path("/resource"sv);
    connection.start_request(1, url);
    connection.start_request(2, url);

    expect_finished_without_error(connection.wait_for_request_to_finish(2, AK::Duration::from_seconds(10)), "not-cached"sv);
    EXPECT_EQ(http_server.connection_count(), 2u);

    // The second connection arrived only after the first had stopped sending: The waiting request held off for as long
    // as the transfer holding its entry kept receiving data.
    EXPECT_EQ(http_server.connection_count_when_finished(0).value_or(0), 1u);
}

// A request whose cache entry is held open by a transfer that's over on the wire, but whose bytes are still going out
// to a slow client, keeps waiting: The entry is still filling — however slowly — so fetching a second copy would waste
// a request on a response that's already on its way to disk.
TEST_CASE(request_waiting_on_a_cache_entry_a_slow_client_is_still_filling_keeps_waiting)
{
    RequestServer::Request::set_wait_for_cache_timeout(AK::Duration::from_seconds(1));
    RequestServer::Request::set_revalidation_stall_timeout(AK::Duration::from_seconds(60));

    TestServer server;
    auto disk_cache = create_test_disk_cache();
    TestConnection connection { server, disk_cache };

    // 8 MiB goes out in 32 KiB pieces, so the transfer is over on the wire in well under a second. The client pipe
    // holds only a small part of that: The rest waits in RequestServer until the client reads what it has.
    auto body = ByteString::repeated('x', 8 * MiB);
    StallingServer http_server { [&](size_t) -> Optional<ServerResponse> {
        return chunked_http_response("max-age=60"sv, body, 32 * KiB, AK::Duration::from_milliseconds(2));
    } };

    auto url = http_server.url_for_path("/resource"sv);
    auto started_at = MonotonicTime::now();
    connection.start_request(1, url);
    connection.start_request(2, url);

    // The client takes 64 KiB every 25ms: three wait limits or so for the whole body, with the entry filling the whole
    // way through — and the request waiting on it has to sit through all of it. And that pace isn't arbitrary:
    // RequestServer writes to the entry only as the pipe takes bytes, and Linux reports a stream socket writable again
    // only once three quarters of its send buffer have drained. So, the client has to take a few hundred KiB well within
    // one limit — or the entry goes quiet for a whole one, and the waiting request rightly gives up on it.
    auto finished = connection.wait_for_request_to_finish_reading_slowly(1, AK::Duration::from_seconds(30), 64 * KiB, AK::Duration::from_milliseconds(25));
    expect_finished_without_error(finished, "written-to-cache"sv);

    // NB: A pipe that could hold the whole body would end the transfer at once — and leave the wait untested.
    EXPECT(MonotonicTime::now() - started_at >= AK::Duration::from_seconds(2));

    expect_finished_without_error(connection.wait_for_request_to_finish(2, AK::Duration::from_seconds(10)), "read-from-cache"sv);
    EXPECT_EQ(http_server.connection_count(), 1u);
}
