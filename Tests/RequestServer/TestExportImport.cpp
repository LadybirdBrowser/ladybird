/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
#include <LibURL/Origin.h>
#include <LibURL/Parser.h>
#include <LibURL/Site.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/ResourceSubstitutionMap.h>

namespace RequestServer {

OwnPtr<ResourceSubstitutionMap> g_resource_substitution_map;

}

namespace {

static void initialize_libcurl()
{
    static bool libcurl_initialized = [] {
        MUST(RequestServer::initialize_libcurl());
        return true;
    }();
    (void)libcurl_initialized;
}

struct TestServer {
    explicit TestServer(StringView name)
    {
        auto cache_root = LexicalPath::join(Core::StandardPaths::cache_directory(), "Ladybird"sv, name);
        disk_cache = MUST(HTTP::DiskCache::create(HTTP::DiskCache::Mode::Testing, cache_root)).release_value();
    }

    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    Optional<HTTP::DiskCache> disk_cache;
};

HTTP::NetworkIsolationKey navigation_key_for(URL::URL const& url)
{
    auto site = URL::Site::serialize_for_partitioning(url.origin()).release_value();
    return HTTP::NetworkIsolationKey {
        .top_level_site = site,
        .frame_site = site,
        .is_cross_site_main_frame_navigation = true,
    };
}

Vector<HTTP::Header> cacheable_request_headers()
{
    return { { ByteString { HTTP::TEST_CACHE_ENABLED_HEADER }, "1"sv } };
}

// A connection passing a response on leaves the body for its recipient, like a renderer with delivery paused.
enum class ReadsBodies {
    No,
    Yes,
};

// A data connection and the messages it receives, read from outside the event loop.
class TestConnection {
public:
    TestConnection(TestServer& server, ReadsBodies reads_bodies = ReadsBodies::Yes, RequestServer::SiteBinding site_binding = RequestServer::SiteBinding::Unrestricted)
        : m_reads_bodies(reads_bodies)
    {
        initialize_libcurl();

        auto pair = MUST(IPC::Transport::create_paired());
        m_remote_transport = MUST(pair.remote_handle.create_transport());
        m_connection = RequestServer::ConnectionFromClient::construct(
            move(pair.local), RequestServer::IsPrivate::No, site_binding,
            server.connections, server.request_transfer_leases, server.disk_cache);
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

    void start_request(u64 request_id, URL::URL url, HTTP::NetworkIsolationKey network_isolation_key, bool create_transfer_lease)
    {
        auto message = make<Messages::RequestServer::StartRequest>(request_id, ByteString { "GET" }, move(url), cacheable_request_headers(), ByteBuffer {}, HTTP::CacheMode::Default, move(network_isolation_key), HTTP::Cookie::IncludeCredentials::No, create_transfer_lease, Optional<u32> {}, false, 0, 0);
        VERIFY(!dispatch(move(message)));
    }

    void adopt_request(int source_client_id, u64 source_request_id, u64 target_request_id, bool preserve_transfer_lease)
    {
        auto message = make<Messages::RequestServer::AdoptRequest>(source_client_id, source_request_id, target_request_id, preserve_transfer_lease);
        VERIFY(!dispatch(move(message)));
    }

    void stop_request(u64 request_id)
    {
        VERIFY(dispatch(make<Messages::RequestServer::StopRequest>(request_id)));
    }

    Optional<Requests::ExportedRequest> export_request(u64 request_id)
    {
        auto response = dispatch(make<Messages::RequestServer::ExportRequest>(request_id));
        if (!response)
            return {};

        Queue<IPC::Attachment> attachments;
        for (auto& attachment : response->take_attachments())
            attachments.enqueue(move(attachment));
        auto message = MUST(RequestServerEndpoint::decode_message(response->data(), attachments));
        VERIFY(message->message_id() == Messages::RequestServer::ExportRequestResponse::static_message_id());
        return message.template release_nonnull<Messages::RequestServer::ExportRequestResponse>()->take_exported_request();
    }

    void import_request(u64 request_id, Requests::ExportedRequest exported, bool create_transfer_lease)
    {
        auto message = make<Messages::RequestServer::ImportRequest>(request_id, move(exported), create_transfer_lease);
        VERIFY(!dispatch(move(message)));
    }

    // Drain response bodies while waiting so the transfer cannot stall on a full pipe.
    template<typename MessageType>
    NonnullOwnPtr<MessageType> wait_for(Core::EventLoop& event_loop, u64 request_id)
    {
        auto wake_timer = Core::Timer::create_repeating(5, [] { });
        wake_timer->start();

        OwnPtr<MessageType> result;
        event_loop.spin_until([&] {
            read_client_messages();
            read_response_bodies();
            for (size_t i = 0; i < m_messages.size(); ++i) {
                if (m_messages[i]->message_id() != MessageType::static_message_id())
                    continue;
                auto message = m_messages.take(i).template release_nonnull<MessageType>();
                if (message->request_id() != request_id)
                    continue;
                result = move(message);
                return true;
            }
            return false;
        });
        return result.release_nonnull();
    }

    ByteBuffer read_whole_body(Core::EventLoop& event_loop, u64 request_id)
    {
        auto wake_timer = Core::Timer::create_repeating(5, [] { });
        wake_timer->start();

        event_loop.spin_until([&] {
            read_client_messages();
            read_response_bodies();
            return m_ended_bodies.contains(request_id);
        });
        return m_bodies.take(request_id).release_value();
    }

    size_t body_size_so_far(u64 request_id) const
    {
        return m_bodies.get(request_id).map([](auto const& body) { return body.size(); }).value_or(0);
    }

    void read_client_messages()
    {
        (void)m_remote_transport->read_as_many_messages_as_possible_without_blocking([&](auto&& raw_message) {
            auto message = MUST(RequestClientEndpoint::decode_message(raw_message.bytes.bytes(), raw_message.attachments));
            if (message->message_id() == Messages::RequestClient::RequestStarted::static_message_id()) {
                auto& started = static_cast<Messages::RequestClient::RequestStarted&>(*message);
                auto socket = MUST(Core::LocalSocket::adopt_fd(started.fd().take_fd()));
                socket->set_notifications_enabled(false);
                MUST(socket->set_blocking(false));
                m_response_sockets.set(started.request_id(), move(socket));
                m_bodies.ensure(started.request_id());
            }
            m_messages.append(move(message));
        });
    }

    void read_response_bodies()
    {
        if (m_reads_bodies == ReadsBodies::No)
            return;

        u8 buffer[64 * KiB];
        Vector<u64> ended;
        for (auto& [request_id, socket] : m_response_sockets) {
            while (true) {
                auto bytes = socket->read_some({ buffer, sizeof(buffer) });
                if (bytes.is_error()) {
                    if (bytes.error().code() != EAGAIN && bytes.error().code() != EWOULDBLOCK)
                        ended.append(request_id);
                    break;
                }
                if (bytes.value().is_empty()) {
                    if (socket->is_eof())
                        ended.append(request_id);
                    break;
                }
                MUST(m_bodies.ensure(request_id).try_append(bytes.value()));
            }
        }
        for (auto request_id : ended) {
            m_response_sockets.remove(request_id);
            m_ended_bodies.set(request_id);
        }
    }

private:
    OwnPtr<IPC::MessageBuffer> dispatch(NonnullOwnPtr<IPC::Message> message)
    {
        return MUST(static_cast<RequestServerEndpoint::Stub&>(*m_connection).handle(move(message)));
    }

    ReadsBodies m_reads_bodies { ReadsBodies::Yes };
    OwnPtr<IPC::Transport> m_remote_transport;
    RefPtr<RequestServer::ConnectionFromClient> m_connection;
    Vector<NonnullOwnPtr<IPC::Message>> m_messages;
    HashMap<u64, NonnullOwnPtr<Core::LocalSocket>> m_response_sockets;
    HashMap<u64, ByteBuffer> m_bodies;
    HashTable<u64> m_ended_bodies;
};

// A local HTTP server that answers every request with the same cacheable body, a piece at a time.
class DrippingServer {
public:
    DrippingServer(ByteBuffer body, size_t piece_size, AK::Duration interval)
        : m_body(move(body))
        , m_piece_size(piece_size)
        , m_interval(interval)
    {
        m_server = MUST(Core::TCPServer::try_create());
        MUST(m_server->listen(IPv4Address { 127, 0, 0, 1 }, 0));
        m_server->on_ready_to_accept = [this] {
            auto socket = MUST(m_server->accept());
            MUST(socket->set_blocking(false));
            ++m_request_count;
            m_connections.append(make<Connection>(move(socket)));
            auto& connection = *m_connections.last();
            connection.socket->on_ready_to_read = [this, &connection] {
                if (!connection.socket->is_open())
                    return;
                u8 buffer[4096];
                auto bytes = connection.socket->read_some({ buffer, sizeof(buffer) });
                if (bytes.is_error() || bytes.value().is_empty()) {
                    connection.socket->on_ready_to_read = nullptr;
                    if (connection.piece_timer)
                        connection.piece_timer->stop();
                    connection.peer_closed = true;
                    return;
                }
                if (connection.started)
                    return;
                connection.started = true;
                MUST(connection.socket->set_blocking(true));
                auto head = ByteString::formatted("HTTP/1.1 200 OK\r\nCache-Control: max-age=600\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", m_body.size());
                MUST(connection.socket->write_until_depleted(head.bytes()));
                MUST(connection.socket->set_blocking(false));
                connection.piece_timer = Core::Timer::create_repeating(static_cast<int>(m_interval.to_milliseconds()), [this, &connection] { write_piece(connection); });
                write_piece(connection);
                connection.piece_timer->start();
            };
        };
    }

    URL::URL url() const
    {
        return URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/document", *m_server->local_port())).release_value();
    }

    size_t request_count() const { return m_request_count; }
    bool peer_closed(size_t index) const { return m_connections[index]->peer_closed; }
    bool finished(size_t index) const { return m_connections[index]->offset == m_body.size(); }

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        explicit Connection(NonnullOwnPtr<Core::TCPSocket> socket)
            : socket(move(socket))
        {
        }

        NonnullOwnPtr<Core::TCPSocket> socket;
        bool started { false };
        bool peer_closed { false };
        size_t offset { 0 };
        RefPtr<Core::Timer> piece_timer;
    };

    void write_piece(Connection& connection)
    {
        // A timer tick may already be queued when the transfer ends.
        if (connection.peer_closed || connection.offset == m_body.size() || !connection.socket->is_open())
            return;
        auto piece = m_body.bytes().slice(connection.offset, min(m_piece_size, m_body.size() - connection.offset));
        MUST(connection.socket->set_blocking(true));
        if (connection.socket->write_until_depleted(piece).is_error()) {
            connection.peer_closed = true;
            connection.piece_timer->stop();
            return;
        }
        MUST(connection.socket->set_blocking(false));
        connection.offset += piece.size();
        if (connection.offset == m_body.size()) {
            connection.piece_timer->stop();
            connection.socket->close();
        }
    }

    RefPtr<Core::TCPServer> m_server;
    Vector<NonnullOwnPtr<Connection>> m_connections;
    ByteBuffer m_body;
    size_t m_piece_size { 0 };
    AK::Duration m_interval;
    size_t m_request_count { 0 };
};

ByteBuffer patterned_body(size_t size)
{
    auto body = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; ++i)
        body[i] = static_cast<u8>('a' + ((i * 7) % 26));
    return body;
}

}

TEST_CASE(exported_response_is_delivered_and_cached_by_the_importing_request_server)
{
    Core::EventLoop event_loop;
    TestServer source_server { "export-source"sv };
    TestServer target_server { "export-target"sv };
    DrippingServer http_server { patterned_body(300 * KiB), 16 * KiB, AK::Duration::from_milliseconds(2) };
    auto url = http_server.url();
    auto key = navigation_key_for(url);

    // Mirror navigation: the document's process fetches the response, then the UI process adopts it.
    TestConnection navigator { source_server, ReadsBodies::No };
    TestConnection source_ui { source_server, ReadsBodies::No };
    navigator.start_request(1, url, key, true);
    (void)navigator.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 1);
    source_ui.adopt_request(navigator.client_id(), 1, 10, true);
    (void)source_ui.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 10);

    // Transfer through the target site's UI client before the target document adopts the response.
    auto exported = source_ui.export_request(10);
    EXPECT(exported.has_value());
    (void)source_ui.wait_for<Messages::RequestClient::RequestTransferred>(event_loop, 10);

    TestConnection target_ui { target_server, ReadsBodies::No };
    TestConnection target { target_server };
    target_ui.import_request(20, exported.release_value(), true);
    (void)target_ui.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 20);
    target.adopt_request(target_ui.client_id(), 20, 30, false);

    auto headers = target.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 30);
    EXPECT_EQ(headers->status_code(), 200u);
    EXPECT(headers->response_headers().first_matching([](auto const& header) { return header.name == "Cache-Control"sv; }).has_value());

    auto finished = target.wait_for<Messages::RequestClient::RequestFinished>(event_loop, 30);
    EXPECT(!finished->network_error().has_value());
    EXPECT_EQ(finished->total_size(), 300 * KiB);

    auto body = target.read_whole_body(event_loop, 30);
    EXPECT_EQ(body.size(), 300 * KiB);
    EXPECT(body == patterned_body(300 * KiB));

    target.start_request(31, url, key, false);
    auto cached_headers = target.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 31);
    auto cache_status = cached_headers->response_headers().first_matching([](auto const& header) { return header.name == HTTP::TEST_CACHE_STATUS_HEADER; });
    EXPECT(cache_status.has_value());
    EXPECT_EQ(cache_status->value, "read-from-cache"sv);
    (void)target.wait_for<Messages::RequestClient::RequestFinished>(event_loop, 31);
    EXPECT_EQ(http_server.request_count(), 1u);
}

TEST_CASE(stopping_an_imported_request_aborts_the_transfer)
{
    Core::EventLoop event_loop;
    TestServer source_server { "abort-source"sv };
    TestServer target_server { "abort-target"sv };
    // A body that takes far longer to arrive than the test runs for.
    DrippingServer http_server { patterned_body(8 * MiB), 4 * KiB, AK::Duration::from_milliseconds(10) };
    auto url = http_server.url();
    auto key = navigation_key_for(url);

    TestConnection navigator { source_server, ReadsBodies::No };
    TestConnection source_ui { source_server, ReadsBodies::No };
    navigator.start_request(1, url, key, true);
    (void)navigator.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 1);
    source_ui.adopt_request(navigator.client_id(), 1, 10, true);
    (void)source_ui.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 10);

    auto exported = source_ui.export_request(10);
    EXPECT(exported.has_value());

    TestConnection target_ui { target_server, ReadsBodies::No };
    TestConnection target { target_server };
    target_ui.import_request(20, exported.release_value(), true);
    target.adopt_request(target_ui.client_id(), 20, 30, false);
    (void)target.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 30);

    event_loop.spin_until([&] {
        target.read_client_messages();
        target.read_response_bodies();
        return target.body_size_so_far(30) >= 16 * KiB;
    });
    target.stop_request(30);

    auto wake_timer = Core::Timer::create_repeating(5, [] { });
    wake_timer->start();
    event_loop.spin_until([&] { return http_server.peer_closed(0); });
    EXPECT(!http_server.finished(0));
}

TEST_CASE(only_unrestricted_clients_may_move_responses)
{
    Core::EventLoop event_loop;
    TestServer server { "bound-export"sv };
    DrippingServer http_server { patterned_body(1 * KiB), 1 * KiB, AK::Duration::from_milliseconds(1) };
    auto url = http_server.url();
    auto key = navigation_key_for(url);

    TestConnection exporter { server, ReadsBodies::No, RequestServer::SiteBinding::Bound };
    EXPECT(!exporter.export_request(1).has_value());
    EXPECT(!exporter.is_open());

    TestConnection navigator { server, ReadsBodies::No };
    navigator.start_request(1, url, key, true);
    (void)navigator.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, 1);
    auto exported = navigator.export_request(1);
    EXPECT(exported.has_value());

    TestConnection importer { server, ReadsBodies::No, RequestServer::SiteBinding::Bound };
    importer.import_request(2, exported.release_value(), true);
    EXPECT(!importer.is_open());
}

TEST_CASE(responses_for_other_sites_are_not_cached_by_a_site_request_server)
{
    Core::EventLoop event_loop;
    TestServer server { "own-site"sv };
    DrippingServer http_server { patterned_body(4 * KiB), 4 * KiB, AK::Duration::from_milliseconds(1) };
    auto url = http_server.url();
    auto key = navigation_key_for(url);

    RequestServer::set_process_top_level_site("https://other.example"_utf16);
    ScopeGuard reset_site = [] { RequestServer::set_process_top_level_site({}); };

    TestConnection client { server };
    for (u64 request_id : { 1u, 2u }) {
        client.start_request(request_id, url, key, false);
        auto headers = client.wait_for<Messages::RequestClient::HeadersBecameAvailable>(event_loop, request_id);
        // A request that never touches the disk cache reports no cache status at all.
        auto cache_status = headers->response_headers().first_matching([](auto const& header) { return header.name == HTTP::TEST_CACHE_STATUS_HEADER; });
        EXPECT(!cache_status.has_value());
        (void)client.wait_for<Messages::RequestClient::RequestFinished>(event_loop, request_id);
    }
    EXPECT_EQ(http_server.request_count(), 2u);
}
