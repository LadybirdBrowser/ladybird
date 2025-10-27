/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Proxy.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibRequests/WebSocket.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Message.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Cache/DiskCache.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/Request.h>
#include <RequestServer/Resolver.h>
#include <RequestServer/WebSocketImplCurl.h>

namespace RequestServer {

static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;

Optional<DiskCache> g_disk_cache;

ConnectionFromClient::ConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint>(*this, move(transport), s_client_ids.allocate())
    , m_resolver(Resolver::default_resolver())
{
    s_connections.set(client_id(), *this);

    m_alt_svc_cache_path = ByteString::formatted("{}/Ladybird/alt-svc-cache.txt", Core::StandardPaths::user_data_directory());

    m_curl_multi = curl_multi_init();

    auto set_option = [this](auto option, auto value) {
        auto result = curl_multi_setopt(m_curl_multi, option, value);
        VERIFY(result == CURLM_OK);
    };
    set_option(CURLMOPT_SOCKETFUNCTION, &on_socket_callback);
    set_option(CURLMOPT_SOCKETDATA, this);
    set_option(CURLMOPT_TIMERFUNCTION, &on_timeout_callback);
    set_option(CURLMOPT_TIMERDATA, this);

    m_timer = Core::Timer::create_single_shot(0, [this] {
        auto result = curl_multi_socket_action(m_curl_multi, CURL_SOCKET_TIMEOUT, 0, nullptr);
        VERIFY(result == CURLM_OK);
        check_active_requests();
    });
}

ConnectionFromClient::~ConnectionFromClient()
{
    m_active_requests.clear();

    curl_multi_cleanup(m_curl_multi);
    m_curl_multi = nullptr;
}

void ConnectionFromClient::request_complete(Badge<Request>, int request_id)
{
    Core::deferred_invoke([weak_self = make_weak_ptr<ConnectionFromClient>(), request_id] {
        if (auto self = weak_self.strong_ref())
            self->m_active_requests.remove(request_id);
    });
}

void ConnectionFromClient::die()
{
    auto client_id = this->client_id();
    s_connections.remove(client_id);
    s_client_ids.deallocate(client_id);

    if (s_connections.is_empty())
        Core::EventLoop::current().quit(0);
}

Messages::RequestServer::InitTransportResponse ConnectionFromClient::init_transport([[maybe_unused]] int peer_pid)
{
#ifdef AK_OS_WINDOWS
    m_transport->set_peer_pid(peer_pid);
    return Core::System::getpid();
#endif
    VERIFY_NOT_REACHED();
}

Messages::RequestServer::ConnectNewClientResponse ConnectionFromClient::connect_new_client()
{
    auto client_socket = create_client_socket();
    if (client_socket.is_error()) {
        dbgln("Failed to create client socket: {}", client_socket.error());
        return IPC::File {};
    }

    return client_socket.release_value();
}

Messages::RequestServer::ConnectNewClientsResponse ConnectionFromClient::connect_new_clients(size_t count)
{
    Vector<IPC::File> files;
    files.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto client_socket = create_client_socket();
        if (client_socket.is_error()) {
            dbgln("Failed to create client socket: {}", client_socket.error());
            return Vector<IPC::File> {};
        }

        files.unchecked_append(client_socket.release_value());
    }

    return files;
}

ErrorOr<IPC::File> ConnectionFromClient::create_client_socket()
{
    // TODO: Mach IPC

    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));

    auto client_socket = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket.is_error()) {
        close(socket_fds[0]);
        close(socket_fds[1]);
        return client_socket.release_error();
    }

    // Note: A ref is stored in the static s_connections map
    auto client = adopt_ref(*new ConnectionFromClient(make<IPC::Transport>(client_socket.release_value())));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::RequestServer::IsSupportedProtocolResponse ConnectionFromClient::is_supported_protocol(ByteString protocol)
{
    return protocol == "http"sv || protocol == "https"sv;
}

void ConnectionFromClient::set_dns_server(ByteString host_or_address, u16 port, bool use_tls, bool validate_dnssec_locally)
{
    auto& dns_info = DNSInfo::the();

    if (host_or_address == dns_info.server_hostname && port == dns_info.port && use_tls == dns_info.use_dns_over_tls && validate_dnssec_locally == dns_info.validate_dnssec_locally)
        return;

    auto result = [&] -> ErrorOr<void> {
        Core::SocketAddress addr;
        if (auto v4 = IPv4Address::from_string(host_or_address); v4.has_value())
            addr = { v4.value(), port };
        else if (auto v6 = IPv6Address::from_string(host_or_address); v6.has_value())
            addr = { v6.value(), port };
        else
            TRY(m_resolver->dns.lookup(host_or_address)->await())->cached_addresses().first().visit([&](auto& address) { addr = { address, port }; });

        dns_info.server_address = addr;
        dns_info.server_hostname = host_or_address;
        dns_info.port = port;
        dns_info.use_dns_over_tls = use_tls;
        dns_info.validate_dnssec_locally = validate_dnssec_locally;
        return {};
    }();

    if (result.is_error())
        dbgln("Failed to set DNS server: {}", result.error());
    else
        m_resolver->dns.reset_connection();
}

void ConnectionFromClient::set_use_system_dns()
{
    auto& dns_info = DNSInfo::the();
    dns_info.server_hostname = {};
    dns_info.server_address = {};

    m_resolver->dns.reset_connection();
}

void ConnectionFromClient::start_request(i32 request_id, ByteString method, URL::URL url, HTTP::HeaderMap request_headers, ByteBuffer request_body, Core::ProxyData proxy_data)
{
    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: start_request({}, {})", request_id, url);

    auto request = Request::fetch(request_id, g_disk_cache, *this, m_curl_multi, m_resolver, move(url), move(method), move(request_headers), move(request_body), m_alt_svc_cache_path, proxy_data);
    m_active_requests.set(request_id, move(request));
}

int ConnectionFromClient::on_socket_callback(CURL*, int sockfd, int what, void* user_data, void*)
{
    auto* client = static_cast<ConnectionFromClient*>(user_data);

    if (what == CURL_POLL_REMOVE) {
        client->m_read_notifiers.remove(sockfd);
        client->m_write_notifiers.remove(sockfd);
        return 0;
    }

    if (what & CURL_POLL_IN) {
        client->m_read_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Read);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_IN, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    if (what & CURL_POLL_OUT) {
        client->m_write_notifiers.ensure(sockfd, [client, sockfd, multi = client->m_curl_multi] {
            auto notifier = Core::Notifier::construct(sockfd, Core::NotificationType::Write);
            notifier->on_activation = [client, sockfd, multi] {
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_OUT, nullptr);
                VERIFY(result == CURLM_OK);

                client->check_active_requests();
            };

            notifier->set_enabled(true);
            return notifier;
        });
    }

    return 0;
}

int ConnectionFromClient::on_timeout_callback(void*, long timeout_ms, void* user_data)
{
    auto* client = static_cast<ConnectionFromClient*>(user_data);
    if (!client->m_timer)
        return 0;

    if (timeout_ms < 0)
        client->m_timer->stop();
    else
        client->m_timer->restart(timeout_ms);

    return 0;
}

void ConnectionFromClient::check_active_requests()
{
    int msgs_in_queue = 0;
    while (auto* msg = curl_multi_info_read(m_curl_multi, &msgs_in_queue)) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        void* application_private = nullptr;
        auto result = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &application_private);
        VERIFY(result == CURLE_OK);
        VERIFY(application_private != nullptr);

        // FIXME: Come up with a unified way to track websockets and standard fetches instead of this nasty tagged pointer
        if (reinterpret_cast<uintptr_t>(application_private) & websocket_private_tag) {
            auto* websocket_impl = reinterpret_cast<WebSocketImplCurl*>(reinterpret_cast<uintptr_t>(application_private) & ~websocket_private_tag);
            if (msg->data.result == CURLE_OK) {
                if (!websocket_impl->did_connect())
                    websocket_impl->on_connection_error();
            } else {
                websocket_impl->on_connection_error();
            }
            continue;
        }

        auto* request = static_cast<Request*>(application_private);
        request->notify_fetch_complete({}, msg->data.result);
    }
}

Messages::RequestServer::StopRequestResponse ConnectionFromClient::stop_request(i32 request_id)
{
    auto request = m_active_requests.take(request_id);
    if (!request.has_value()) {
        dbgln("StopRequest: Request ID {} not found", request_id);
        return false;
    }

    return true;
}

Messages::RequestServer::SetCertificateResponse ConnectionFromClient::set_certificate(i32 request_id, ByteString certificate, ByteString key)
{
    (void)request_id;
    (void)certificate;
    (void)key;
    TODO();
}

void ConnectionFromClient::ensure_connection(URL::URL url, ::RequestServer::CacheLevel cache_level)
{
    auto connect_only_request_id = get_random<i32>();

    auto request = Request::connect(connect_only_request_id, *this, m_curl_multi, m_resolver, move(url), cache_level);
    m_active_requests.set(connect_only_request_id, move(request));
}

void ConnectionFromClient::clear_cache()
{
    if (g_disk_cache.has_value())
        g_disk_cache->clear_cache();
}

void ConnectionFromClient::websocket_connect(i64 websocket_id, URL::URL url, ByteString origin, Vector<ByteString> protocols, Vector<ByteString> extensions, HTTP::HeaderMap additional_request_headers)
{
    auto host = url.serialized_host().to_byte_string();

    m_resolver->dns.lookup(host, DNS::Messages::Class::IN, { DNS::Messages::ResourceType::A, DNS::Messages::ResourceType::AAAA })
        ->when_rejected([this, websocket_id](auto const& error) {
            dbgln("WebSocketConnect: DNS lookup failed: {}", error);
            async_websocket_errored(websocket_id, static_cast<i32>(Requests::WebSocket::Error::CouldNotEstablishConnection));
        })
        .when_resolved([this, websocket_id, host = move(host), url = move(url), origin = move(origin), protocols = move(protocols), extensions = move(extensions), additional_request_headers = move(additional_request_headers)](auto const& dns_result) mutable {
            if (dns_result->is_empty() || !dns_result->has_cached_addresses()) {
                dbgln("WebSocketConnect: DNS lookup failed for '{}'", host);
                async_websocket_errored(websocket_id, static_cast<i32>(Requests::WebSocket::Error::CouldNotEstablishConnection));
                return;
            }

            WebSocket::ConnectionInfo connection_info(move(url));
            connection_info.set_origin(move(origin));
            connection_info.set_protocols(move(protocols));
            connection_info.set_extensions(move(extensions));
            connection_info.set_headers(move(additional_request_headers));
            connection_info.set_dns_result(move(dns_result));

            if (auto const& path = default_certificate_path(); !path.is_empty())
                connection_info.set_root_certificates_path(path);

            auto impl = WebSocketImplCurl::create(m_curl_multi);
            auto connection = WebSocket::WebSocket::create(move(connection_info), move(impl));

            connection->on_open = [this, websocket_id]() {
                async_websocket_connected(websocket_id);
            };
            connection->on_message = [this, websocket_id](auto message) {
                async_websocket_received(websocket_id, message.is_text(), message.data());
            };
            connection->on_error = [this, websocket_id](auto message) {
                async_websocket_errored(websocket_id, (i32)message);
            };
            connection->on_close = [this, websocket_id](u16 code, ByteString reason, bool was_clean) {
                async_websocket_closed(websocket_id, code, move(reason), was_clean);
            };
            connection->on_ready_state_change = [this, websocket_id](auto state) {
                async_websocket_ready_state_changed(websocket_id, (u32)state);
            };

            connection->start();
            m_websockets.set(websocket_id, move(connection));
        });
}

void ConnectionFromClient::websocket_send(i64 websocket_id, bool is_text, ByteBuffer data)
{
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->send(WebSocket::Message { move(data), is_text });
}

void ConnectionFromClient::websocket_close(i64 websocket_id, u16 code, ByteString reason)
{
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->close(code, reason);
}

Messages::RequestServer::WebsocketSetCertificateResponse ConnectionFromClient::websocket_set_certificate(i64 websocket_id, ByteString, ByteString)
{
    auto success = false;
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection) {
        // NO OP here
        // connection->set_certificate(certificate, key);
        success = true;
    }
    return success;
}

}
