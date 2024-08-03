/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/IDAllocator.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Weakable.h>
#include <LibCore/Proxy.h>
#include <LibCore/Socket.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Message.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Protocol.h>
#include <RequestServer/Request.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <netdb.h>

namespace RequestServer {

template<typename Pool>
struct Looper : public Threading::ThreadPoolLooper<Pool> {
    IterationDecision next(Pool& pool, bool wait);
    Core::EventLoop event_loop;
};

struct ThreadPoolEntry {
    NonnullRefPtr<ConnectionFromClient> client;
    ConnectionFromClient::Work work;
};
static Threading::ThreadPool<ThreadPoolEntry, Looper> s_thread_pool {
    [](ThreadPoolEntry entry) {
        entry.client->worker_do_work(move(entry.work));
    }
};

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint>(*this, move(socket), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);
}

ConnectionFromClient::~ConnectionFromClient()
{
    m_requests.with_locked([](HashMap<i32, OwnPtr<Request>>& map) {
        for (auto& entry : map)
            entry.value->cancel();
    });
}

class Job : public RefCounted<Job>
    , public Weakable<Job> {
public:
    static NonnullRefPtr<Job> ensure(URL::URL const& url)
    {
        RefPtr<Job> job;
        if (auto it = s_jobs.find(url); it != s_jobs.end())
            job = it->value.strong_ref();
        if (job == nullptr) {
            job = adopt_ref(*new Job(url));
            s_jobs.set(url, job);
        }
        return *job;
    }

    void start(Core::Socket& socket)
    {
        auto is_connected = socket.is_open();
        VERIFY(is_connected);
        ConnectionCache::request_did_finish(m_url, &socket);
    }

    void fail(Core::NetworkJob::Error error)
    {
        dbgln("Pre-connect to {} failed: {}", m_url, Core::to_string(error));
    }

    void will_be_destroyed() const
    {
        s_jobs.remove(m_url);
    }

private:
    explicit Job(URL::URL url)
        : m_url(move(url))
    {
    }

    URL::URL m_url;
    inline static HashMap<URL::URL, WeakPtr<Job>> s_jobs {};
};

template<typename Pool>
IterationDecision Looper<Pool>::next(Pool& pool, bool wait)
{
    bool should_exit = false;
    auto timer = Core::Timer::create_repeating(100, [&] {
        if (Threading::ThreadPoolLooper<Pool>::next(pool, false) == IterationDecision::Break) {
            event_loop.quit(0);
            should_exit = true;
        }
    });

    timer->start();
    if (!wait) {
        event_loop.deferred_invoke([&] {
            event_loop.quit(0);
        });
    }

    event_loop.exec();

    if (should_exit)
        return IterationDecision::Break;
    return IterationDecision::Continue;
}

void ConnectionFromClient::worker_do_work(Work work)
{
    work.visit(
        [&](StartRequest& start_request) {
            auto* protocol = Protocol::find_by_name(start_request.url.scheme().to_byte_string());
            if (!protocol) {
                dbgln("StartRequest: No protocol handler for URL: '{}'", start_request.url);
                auto lock = Threading::MutexLocker(m_ipc_mutex);
                (void)post_message(Messages::RequestClient::RequestFinished(start_request.request_id, false, 0));
                return;
            }
            auto request = protocol->start_request(start_request.request_id, *this, start_request.method, start_request.url, start_request.request_headers, start_request.request_body, start_request.proxy_data);
            if (!request) {
                dbgln("StartRequest: Protocol handler failed to start request: '{}'", start_request.url);
                auto lock = Threading::MutexLocker(m_ipc_mutex);
                (void)post_message(Messages::RequestClient::RequestFinished(start_request.request_id, false, 0));
                return;
            }
            auto id = request->id();
            auto fd = request->request_fd();
            m_requests.with_locked([&](auto& map) { map.set(id, move(request)); });
            auto lock = Threading::MutexLocker(m_ipc_mutex);
            (void)post_message(Messages::RequestClient::RequestStarted(start_request.request_id, IPC::File::adopt_fd(fd)));
        },
        [&](EnsureConnection& ensure_connection) {
            auto& url = ensure_connection.url;
            auto& cache_level = ensure_connection.cache_level;

            if (cache_level == CacheLevel::ResolveOnly) {
                Core::deferred_invoke([host = url.serialized_host().release_value_but_fixme_should_propagate_errors().to_byte_string()] {
                    dbgln("EnsureConnection: DNS-preload for {}", host);
                    auto resolved_host = Core::Socket::resolve_host(host, Core::Socket::SocketType::Stream);
                    if (resolved_host.is_error())
                        dbgln("EnsureConnection: DNS-preload failed for {}", host);
                });
                dbgln("EnsureConnection: DNS-preload for {} done", url);
                return;
            }

            dbgln("EnsureConnection: Pre-connect to {}", url);
            auto do_preconnect = [=, job = Job::ensure(url)](auto& cache) {
                ConnectionCache::ensure_connection(cache, url, move(job));
            };

            if (url.scheme() == "http"sv)
                do_preconnect(ConnectionCache::g_tcp_connection_cache);
            else if (url.scheme() == "https"sv)
                do_preconnect(ConnectionCache::g_tls_connection_cache);
            else
                dbgln("EnsureConnection: Invalid URL scheme: '{}'", url.scheme());
        },
        [&](Empty) {});
}

void ConnectionFromClient::die()
{
    auto client_id = this->client_id();
    s_connections.remove(client_id);
    s_client_ids.deallocate(client_id);

    if (s_connections.is_empty())
        Core::EventLoop::current().quit(0);
}

Messages::RequestServer::ConnectNewClientResponse ConnectionFromClient::connect_new_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error()) {
        dbgln("Failed to create client socketpair: {}", err.error());
        return IPC::File {};
    }

    auto client_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket_or_error.is_error()) {
        close(socket_fds[0]);
        close(socket_fds[1]);
        dbgln("Failed to adopt client socket: {}", client_socket_or_error.error());
        return IPC::File {};
    }
    auto client_socket = client_socket_or_error.release_value();
    // Note: A ref is stored in the static s_connections map
    auto client = adopt_ref(*new ConnectionFromClient(move(client_socket)));

    return IPC::File::adopt_fd(socket_fds[1]);
}

void ConnectionFromClient::enqueue(Work work)
{
    s_thread_pool.submit({ *this, move(work) });
}

Messages::RequestServer::IsSupportedProtocolResponse ConnectionFromClient::is_supported_protocol(ByteString const& protocol)
{
    bool supported = Protocol::find_by_name(protocol.to_lowercase());
    return supported;
}

void ConnectionFromClient::start_request(i32 request_id, ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& request_headers, ByteBuffer const& request_body, Core::ProxyData const& proxy_data)
{
    if (!url.is_valid()) {
        dbgln("StartRequest: Invalid URL requested: '{}'", url);
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        (void)post_message(Messages::RequestClient::RequestFinished(request_id, false, 0));
        return;
    }

    auto headers = request_headers;
    if (!headers.contains("Accept-Encoding"))
        headers.set("Accept-Encoding", "gzip, deflate, br");

    enqueue(StartRequest {
        .request_id = request_id,
        .method = method,
        .url = url,
        .request_headers = move(headers),
        .request_body = request_body,
        .proxy_data = proxy_data,
    });
}

Messages::RequestServer::StopRequestResponse ConnectionFromClient::stop_request(i32 request_id)
{
    return m_requests.with_locked([&](auto& map) {
        auto* request = const_cast<Request*>(map.get(request_id).value_or(nullptr));
        bool success = false;
        if (request) {
            request->stop();
            map.remove(request_id);
            success = true;
        }
        return success;
    });
}

void ConnectionFromClient::did_receive_headers(Badge<Request>, Request& request)
{
    auto lock = Threading::MutexLocker(m_ipc_mutex);
    async_headers_became_available(request.id(), request.response_headers(), request.status_code());
}

void ConnectionFromClient::did_finish_request(Badge<Request>, Request& request, bool success)
{
    if (request.total_size().has_value()) {
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        async_request_finished(request.id(), success, request.total_size().value());
    }

    m_requests.with_locked([&](auto& map) { map.remove(request.id()); });
}

void ConnectionFromClient::did_progress_request(Badge<Request>, Request& request)
{
    auto lock = Threading::MutexLocker(m_ipc_mutex);
    async_request_progress(request.id(), request.total_size(), request.downloaded_size());
}

void ConnectionFromClient::did_request_certificates(Badge<Request>, Request& request)
{
    auto lock = Threading::MutexLocker(m_ipc_mutex);
    async_certificate_requested(request.id());
}

Messages::RequestServer::SetCertificateResponse ConnectionFromClient::set_certificate(i32 request_id, ByteString const& certificate, ByteString const& key)
{
    return m_requests.with_locked([&](auto& map) {
        auto* request = const_cast<Request*>(map.get(request_id).value_or(nullptr));
        bool success = false;
        if (request) {
            request->set_certificate(certificate, key);
            success = true;
        }
        return success;
    });
}

void ConnectionFromClient::ensure_connection(URL::URL const& url, ::RequestServer::CacheLevel const& cache_level)
{
    if (!url.is_valid()) {
        dbgln("EnsureConnection: Invalid URL requested: '{}'", url);
        return;
    }

    enqueue(EnsureConnection {
        .url = url,
        .cache_level = cache_level,
    });
}

static i32 s_next_websocket_id = 1;
Messages::RequestServer::WebsocketConnectResponse ConnectionFromClient::websocket_connect(URL::URL const& url, ByteString const& origin, Vector<ByteString> const& protocols, Vector<ByteString> const& extensions, HTTP::HeaderMap const& additional_request_headers)
{
    if (!url.is_valid()) {
        dbgln("WebSocket::Connect: Invalid URL requested: '{}'", url);
        return -1;
    }

    WebSocket::ConnectionInfo connection_info(url);
    connection_info.set_origin(origin);
    connection_info.set_protocols(protocols);
    connection_info.set_extensions(extensions);
    connection_info.set_headers(additional_request_headers);

    auto id = ++s_next_websocket_id;
    auto connection = WebSocket::WebSocket::create(move(connection_info));
    connection->on_open = [this, id]() {
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        async_websocket_connected(id);
    };
    connection->on_message = [this, id](auto message) {
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        async_websocket_received(id, message.is_text(), message.data());
    };
    connection->on_error = [this, id](auto message) {
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        async_websocket_errored(id, (i32)message);
    };
    connection->on_close = [this, id](u16 code, ByteString reason, bool was_clean) {
        auto lock = Threading::MutexLocker(m_ipc_mutex);
        async_websocket_closed(id, code, move(reason), was_clean);
    };

    connection->start();
    m_websockets.set(id, move(connection));
    return id;
}

Messages::RequestServer::WebsocketReadyStateResponse ConnectionFromClient::websocket_ready_state(i32 connection_id)
{
    if (auto connection = m_websockets.get(connection_id).value_or({}))
        return (u32)connection->ready_state();
    return (u32)WebSocket::ReadyState::Closed;
}

Messages::RequestServer::WebsocketSubprotocolInUseResponse ConnectionFromClient::websocket_subprotocol_in_use(i32 connection_id)
{
    if (auto connection = m_websockets.get(connection_id).value_or({}))
        return connection->subprotocol_in_use();
    return ByteString::empty();
}

void ConnectionFromClient::websocket_send(i32 connection_id, bool is_text, ByteBuffer const& data)
{
    if (auto connection = m_websockets.get(connection_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->send(WebSocket::Message { data, is_text });
}

void ConnectionFromClient::websocket_close(i32 connection_id, u16 code, ByteString const& reason)
{
    if (auto connection = m_websockets.get(connection_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->close(code, reason);
}

Messages::RequestServer::WebsocketSetCertificateResponse ConnectionFromClient::websocket_set_certificate(i32 connection_id, ByteString const&, ByteString const&)
{
    auto success = false;
    if (auto connection = m_websockets.get(connection_id).value_or({}); connection) {
        // NO OP here
        // connection->set_certificate(certificate, key);
        success = true;
    }
    return success;
}

void ConnectionFromClient::dump_connection_info()
{
    ConnectionCache::dump_jobs();
}

}
