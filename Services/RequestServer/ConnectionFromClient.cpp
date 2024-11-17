/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/IDAllocator.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Proxy.h>
#include <LibCore/Socket.h>
#include <LibRequests/NetworkErrorEnum.h>
#include <LibTextCodec/Decoder.h>
#include <LibWebSocket/ConnectionInfo.h>
#include <LibWebSocket/Message.h>
#include <RequestServer/ConnectionFromClient.h>
#include <RequestServer/RequestClientEndpoint.h>
#include <curl/curl.h>

namespace RequestServer {

ByteString g_default_certificate_path;
static HashMap<int, RefPtr<ConnectionFromClient>> s_connections;
static IDAllocator s_client_ids;
static long s_connect_timeout_seconds = 90L;

struct ConnectionFromClient::ActiveRequest {
    CURLM* multi { nullptr };
    CURL* easy { nullptr };
    i32 request_id { 0 };
    RefPtr<Core::Notifier> notifier;
    WeakPtr<ConnectionFromClient> client;
    int writer_fd { 0 };
    HTTP::HeaderMap headers;
    bool got_all_headers { false };
    bool is_connect_only { false };
    size_t downloaded_so_far { 0 };
    String url;
    Optional<String> reason_phrase;
    ByteBuffer body;

    ActiveRequest(ConnectionFromClient& client, CURLM* multi, CURL* easy, i32 request_id, int writer_fd)
        : multi(multi)
        , easy(easy)
        , request_id(request_id)
        , client(client)
        , writer_fd(writer_fd)
    {
    }

    ~ActiveRequest()
    {
        if (writer_fd > 0)
            MUST(Core::System::close(writer_fd));

        auto result = curl_multi_remove_handle(multi, easy);
        VERIFY(result == CURLM_OK);
        curl_easy_cleanup(easy);
    }

    void flush_headers_if_needed()
    {
        if (got_all_headers)
            return;
        got_all_headers = true;
        long http_status_code = 0;
        auto result = curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status_code);
        VERIFY(result == CURLE_OK);
        client->async_headers_became_available(request_id, headers, http_status_code, reason_phrase);
    }
};

size_t ConnectionFromClient::on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto* request = static_cast<ActiveRequest*>(user_data);
    size_t total_size = size * nmemb;
    auto header_line = StringView { static_cast<char const*>(buffer), total_size };

    // NOTE: We need to extract the HTTP reason phrase since it can be a custom value.
    //       Fetching infrastructure needs this value for setting the status message.
    if (!request->reason_phrase.has_value() && header_line.starts_with("HTTP/"sv)) {
        if (auto const space_positions = header_line.find_all(" "sv); space_positions.size() > 1) {
            auto const second_space_offset = space_positions.at(1);
            auto const reason_phrase_string_view = header_line.substring_view(second_space_offset + 1).trim_whitespace();

            if (!reason_phrase_string_view.is_empty()) {
                auto decoder = TextCodec::decoder_for_exact_name("ISO-8859-1"sv);
                VERIFY(decoder.has_value());

                request->reason_phrase = MUST(decoder->to_utf8(reason_phrase_string_view));
                return total_size;
            }
        }
    }

    if (auto colon_index = header_line.find(':'); colon_index.has_value()) {
        auto name = header_line.substring_view(0, colon_index.value()).trim_whitespace();
        auto value = header_line.substring_view(colon_index.value() + 1, header_line.length() - colon_index.value() - 1).trim_whitespace();
        request->headers.set(name, value);
    }

    return total_size;
}

size_t ConnectionFromClient::on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data)
{
    auto* request = static_cast<ActiveRequest*>(user_data);
    request->flush_headers_if_needed();

    size_t total_size = size * nmemb;

    size_t remaining_length = total_size;
    u8 const* remaining_data = static_cast<u8 const*>(buffer);
    while (remaining_length > 0) {
        auto result = Core::System::write(request->writer_fd, { remaining_data, remaining_length });
        if (result.is_error()) {
            if (result.error().code() != EAGAIN) {
                dbgln("on_data_received: write failed: {}", result.error());
                VERIFY_NOT_REACHED();
            }
            sched_yield();
            continue;
        }
        auto nwritten = result.value();
        if (nwritten == 0) {
            dbgln("on_data_received: write returned 0");
            VERIFY_NOT_REACHED();
        }
        remaining_data += nwritten;
        remaining_length -= nwritten;
    }

    Optional<u64> content_length_for_ipc;
    curl_off_t content_length = -1;
    auto res = curl_easy_getinfo(request->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
    if (res == CURLE_OK && content_length != -1) {
        content_length_for_ipc = content_length;
    }

    request->downloaded_so_far += total_size;

    return total_size;
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
                int still_running = 0;
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_IN, &still_running);
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
                int still_running = 0;
                auto result = curl_multi_socket_action(multi, sockfd, CURL_CSELECT_OUT, &still_running);
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
    if (timeout_ms < 0) {
        client->m_timer->stop();
    } else {
        client->m_timer->restart(timeout_ms);
    }
    return 0;
}

ConnectionFromClient::ConnectionFromClient(IPC::Transport transport)
    : IPC::ConnectionFromClient<RequestClientEndpoint, RequestServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    s_connections.set(client_id(), *this);

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
        int still_running = 0;
        auto result = curl_multi_socket_action(m_curl_multi, CURL_SOCKET_TIMEOUT, 0, &still_running);
        VERIFY(result == CURLM_OK);
        check_active_requests();
    });
}

ConnectionFromClient::~ConnectionFromClient()
{
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
    static_assert(IsSame<IPC::Transport, IPC::TransportSocket>, "Need to handle other IPC transports here");

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
    auto client = adopt_ref(*new ConnectionFromClient(IPC::Transport(move(client_socket))));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::RequestServer::IsSupportedProtocolResponse ConnectionFromClient::is_supported_protocol(ByteString const& protocol)
{
    return protocol == "http"sv || protocol == "https"sv;
}

void ConnectionFromClient::start_request(i32 request_id, ByteString const& method, URL::URL const& url, HTTP::HeaderMap const& request_headers, ByteBuffer const& request_body, Core::ProxyData const& proxy_data)
{
    if (!url.is_valid()) {
        dbgln("StartRequest: Invalid URL requested: '{}'", url);
        async_request_finished(request_id, 0, Requests::NetworkError::MalformedUrl);
        return;
    }

    auto* easy = curl_easy_init();
    if (!easy) {
        dbgln("StartRequest: Failed to initialize curl easy handle");
        return;
    }

    auto fds_or_error = Core::System::pipe2(O_NONBLOCK);
    if (fds_or_error.is_error()) {
        dbgln("StartRequest: Failed to create pipe: {}", fds_or_error.error());
        return;
    }

    auto fds = fds_or_error.release_value();
    auto writer_fd = fds[1];
    auto reader_fd = fds[0];
    async_request_started(request_id, IPC::File::adopt_fd(reader_fd));

    auto request = make<ActiveRequest>(*this, m_curl_multi, easy, request_id, writer_fd);
    request->url = url.to_string().value();

    auto set_option = [easy](auto option, auto value) {
        auto result = curl_easy_setopt(easy, option, value);
        if (result != CURLE_OK) {
            dbgln("StartRequest: Failed to set curl option: {}", curl_easy_strerror(result));
            return false;
        }
        return true;
    };

    set_option(CURLOPT_PRIVATE, request.ptr());

    if (!g_default_certificate_path.is_empty())
        set_option(CURLOPT_CAINFO, g_default_certificate_path.characters());

    set_option(CURLOPT_ACCEPT_ENCODING, "gzip, deflate, br");
    set_option(CURLOPT_URL, url.to_string().value().to_byte_string().characters());
    set_option(CURLOPT_PORT, url.port_or_default());
    set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);

    bool did_set_body = false;

    if (method == "GET"sv) {
        set_option(CURLOPT_HTTPGET, 1L);
    } else if (method.is_one_of("POST"sv, "PUT"sv, "PATCH"sv, "DELETE"sv)) {
        request->body = request_body;
        set_option(CURLOPT_POSTFIELDSIZE, request->body.size());
        set_option(CURLOPT_POSTFIELDS, request->body.data());
        did_set_body = true;
    } else if (method == "HEAD") {
        set_option(CURLOPT_NOBODY, 1L);
    }
    set_option(CURLOPT_CUSTOMREQUEST, method.characters());

    set_option(CURLOPT_FOLLOWLOCATION, 0);

    struct curl_slist* curl_headers = nullptr;

    // NOTE: CURLOPT_POSTFIELDS automatically sets the Content-Type header.
    //       Set it to empty if the headers passed in don't contain a content type.
    if (did_set_body && !request_headers.contains("Content-Type"))
        curl_headers = curl_slist_append(curl_headers, "Content-Type:");

    for (auto const& header : request_headers.headers()) {
        auto header_string = ByteString::formatted("{}: {}", header.name, header.value);
        curl_headers = curl_slist_append(curl_headers, header_string.characters());
    }
    set_option(CURLOPT_HTTPHEADER, curl_headers);

    // FIXME: Set up proxy if applicable
    (void)proxy_data;

    set_option(CURLOPT_WRITEFUNCTION, &on_data_received);
    set_option(CURLOPT_WRITEDATA, reinterpret_cast<void*>(request.ptr()));

    set_option(CURLOPT_HEADERFUNCTION, &on_header_received);
    set_option(CURLOPT_HEADERDATA, reinterpret_cast<void*>(request.ptr()));

    auto result = curl_multi_add_handle(m_curl_multi, easy);
    VERIFY(result == CURLM_OK);

    m_active_requests.set(request_id, move(request));
}

static Requests::NetworkError map_curl_code_to_network_error(CURLcode const& code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return Requests::NetworkError::UnableToResolveHost;
    case CURLE_COULDNT_RESOLVE_PROXY:
        return Requests::NetworkError::UnableToResolveProxy;
    case CURLE_COULDNT_CONNECT:
        return Requests::NetworkError::UnableToConnect;
    case CURLE_OPERATION_TIMEDOUT:
        return Requests::NetworkError::TimeoutReached;
    case CURLE_TOO_MANY_REDIRECTS:
        return Requests::NetworkError::TooManyRedirects;
    case CURLE_SSL_CONNECT_ERROR:
        return Requests::NetworkError::SSLHandshakeFailed;
    case CURLE_PEER_FAILED_VERIFICATION:
        return Requests::NetworkError::SSLVerificationFailed;
    case CURLE_URL_MALFORMAT:
        return Requests::NetworkError::MalformedUrl;
    default:
        return Requests::NetworkError::Unknown;
    }
}

void ConnectionFromClient::check_active_requests()
{
    int msgs_in_queue = 0;
    while (auto* msg = curl_multi_info_read(m_curl_multi, &msgs_in_queue)) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        ActiveRequest* request = nullptr;
        auto result = curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &request);
        VERIFY(result == CURLE_OK);

        if (!request->is_connect_only) {
            request->flush_headers_if_needed();

            auto result_code = msg->data.result;

            Optional<Requests::NetworkError> network_error;
            bool const request_was_successful = result_code == CURLE_OK;
            if (!request_was_successful) {
                network_error = map_curl_code_to_network_error(result_code);

                if (network_error.has_value() && network_error.value() == Requests::NetworkError::Unknown) {
                    char const* curl_error_message = curl_easy_strerror(result_code);
                    dbgln("ConnectionFromClient: Unable to map error ({}), message: \"\033[31;1m{}\033[0m\"", static_cast<int>(result_code), curl_error_message);
                }
            }

            async_request_finished(request->request_id, request->downloaded_so_far, network_error);
        }

        m_active_requests.remove(request->request_id);
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

void ConnectionFromClient::did_receive_headers(Badge<Request>, Request&)
{
}

void ConnectionFromClient::did_finish_request(Badge<Request>, Request&, bool)
{
}

void ConnectionFromClient::did_progress_request(Badge<Request>, Request&)
{
}

void ConnectionFromClient::did_request_certificates(Badge<Request>, Request&)
{
    TODO();
}

Messages::RequestServer::SetCertificateResponse ConnectionFromClient::set_certificate(i32 request_id, ByteString const& certificate, ByteString const& key)
{
    (void)request_id;
    (void)certificate;
    (void)key;
    TODO();
}

void ConnectionFromClient::ensure_connection(URL::URL const& url, ::RequestServer::CacheLevel const& cache_level)
{
    if (!url.is_valid()) {
        dbgln("EnsureConnection: Invalid URL requested: '{}'", url);
        return;
    }

    auto const url_string_value = url.to_string().value();

    if (cache_level == CacheLevel::CreateConnection) {
        auto* easy = curl_easy_init();
        if (!easy) {
            dbgln("EnsureConnection: Failed to initialize curl easy handle");
            return;
        }

        auto set_option = [easy](auto option, auto value) {
            auto result = curl_easy_setopt(easy, option, value);
            if (result != CURLE_OK) {
                dbgln("EnsureConnection: Failed to set curl option: {}", curl_easy_strerror(result));
                return false;
            }
            return true;
        };

        auto connect_only_request_id = get_random<i32>();

        auto request = make<ActiveRequest>(*this, m_curl_multi, easy, connect_only_request_id, 0);
        request->url = url_string_value;
        request->is_connect_only = true;

        set_option(CURLOPT_PRIVATE, request.ptr());
        set_option(CURLOPT_URL, url_string_value.to_byte_string().characters());
        set_option(CURLOPT_PORT, url.port_or_default());
        set_option(CURLOPT_CONNECTTIMEOUT, s_connect_timeout_seconds);
        set_option(CURLOPT_CONNECT_ONLY, 1L);

        auto const result = curl_multi_add_handle(m_curl_multi, easy);
        VERIFY(result == CURLM_OK);

        m_active_requests.set(connect_only_request_id, move(request));

        return;
    }

    if (cache_level == CacheLevel::ResolveOnly) {
        dbgln("FIXME: EnsureConnection: Implement ResolveOnly cache level");
    }
}

void ConnectionFromClient::websocket_connect(i64 websocket_id, URL::URL const& url, ByteString const& origin, Vector<ByteString> const& protocols, Vector<ByteString> const& extensions, HTTP::HeaderMap const& additional_request_headers)
{
    if (!url.is_valid()) {
        dbgln("WebSocket::Connect: Invalid URL requested: '{}'", url);
        return;
    }

    WebSocket::ConnectionInfo connection_info(url);
    connection_info.set_origin(origin);
    connection_info.set_protocols(protocols);
    connection_info.set_extensions(extensions);
    connection_info.set_headers(additional_request_headers);

    auto connection = WebSocket::WebSocket::create(move(connection_info));
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
}

void ConnectionFromClient::websocket_send(i64 websocket_id, bool is_text, ByteBuffer const& data)
{
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->send(WebSocket::Message { data, is_text });
}

void ConnectionFromClient::websocket_close(i64 websocket_id, u16 code, ByteString const& reason)
{
    if (auto connection = m_websockets.get(websocket_id).value_or({}); connection && connection->ready_state() == WebSocket::ReadyState::Open)
        connection->close(code, reason);
}

Messages::RequestServer::WebsocketSetCertificateResponse ConnectionFromClient::websocket_set_certificate(i64 websocket_id, ByteString const&, ByteString const&)
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
