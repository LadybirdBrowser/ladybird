/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/MemoryStream.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCore/Proxy.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/Cache/CacheRequest.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <RequestServer/CacheLevel.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestPipe.h>

struct curl_slist;

namespace RequestServer {

class Request final : public HTTP::CacheRequest {
public:
    static NonnullOwnPtr<Request> fetch(
        u64 request_id,
        Optional<HTTP::DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    static NonnullOwnPtr<Request> connect(
        u64 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        CacheLevel cache_level);

    static NonnullOwnPtr<Request> revalidate(
        u64 request_id,
        Optional<HTTP::DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    virtual ~Request() override;

    enum class Type : u8 {
        Fetch,
        Connect,
        BackgroundRevalidation,
    };

    u64 request_id() const { return m_request_id; }
    Type type() const { return m_type; }

    URL::URL const& url() const { return m_url; }
    ByteString const& method() const { return m_method; }
    HTTP::HeaderList const& request_headers() const { return m_request_headers; }
    UnixDateTime request_start_time() const { return m_request_start_time; }

    virtual void notify_request_unblocked(Badge<HTTP::DiskCache>) override;
    void notify_fetch_complete(Badge<ConnectionFromClient>, int result_code);

private:
    enum class State : u8 {
        Init,         // Decide whether to service this request from cache or the network.
        ReadCache,    // Read the cached response from disk.
        WaitForCache, // Wait for an existing cache entry to complete before proceeding.
        DNSLookup,    // Resolve the URL's host.
        Connect,      // Issue a network request to connect to the URL.
        Fetch,        // Issue a network request to fetch the URL.
        Complete,     // Finalize the request with the client.
        Error,        // Any error occured during the request's lifetime.
    };

    static constexpr StringView state_name(State state)
    {
        switch (state) {
        case State::Init:
            return "Init"sv;
        case State::ReadCache:
            return "ReadCache"sv;
        case State::WaitForCache:
            return "WaitForCache"sv;
        case State::DNSLookup:
            return "DNSLookup"sv;
        case State::Connect:
            return "Connect"sv;
        case State::Fetch:
            return "Fetch"sv;
        case State::Complete:
            return "Complete"sv;
        case State::Error:
            return "Error"sv;
        }
        VERIFY_NOT_REACHED();
    }

    Request(
        u64 request_id,
        Type type,
        Optional<HTTP::DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        NonnullRefPtr<HTTP::HeaderList> request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    Request(
        u64 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url);

    void transition_to_state(State);
    void process();

    void handle_initial_state();
    void handle_read_cache_state();
    void handle_dns_lookup_state();
    void handle_connect_state();
    void handle_fetch_state();
    void handle_complete_state();
    void handle_error_state();

    static size_t on_header_received(void* buffer, size_t size, size_t nmemb, void* user_data);
    static size_t on_data_received(void* buffer, size_t size, size_t nmemb, void* user_data);

    ErrorOr<void> inform_client_request_started();
    void transfer_headers_to_client_if_needed();
    ErrorOr<void> write_queued_bytes_without_blocking();

    virtual bool is_revalidation_request() const override;
    ErrorOr<void> revalidation_failed();

    u32 acquire_status_code() const;
    Requests::RequestTimingInfo acquire_timing_info() const;

    u64 m_request_id { 0 };
    Type m_type { Type::Fetch };
    State m_state { State::Init };

    Optional<HTTP::DiskCache&> m_disk_cache;
    ConnectionFromClient& m_client;

    void* m_curl_multi_handle { nullptr };
    void* m_curl_easy_handle { nullptr };
    Vector<curl_slist*> m_curl_string_lists;
    Optional<int> m_curl_result_code;

    NonnullRefPtr<Resolver> m_resolver;
    RefPtr<DNS::LookupResult const> m_dns_result;

    URL::URL m_url;
    ByteString m_method;

    UnixDateTime m_request_start_time { UnixDateTime::now() };
    NonnullRefPtr<HTTP::HeaderList> m_request_headers;
    ByteBuffer m_request_body;

    ByteString m_alt_svc_cache_path;
    Core::ProxyData m_proxy_data;

    u32 m_status_code { 0 };
    Optional<String> m_reason_phrase;

    NonnullRefPtr<HTTP::HeaderList> m_response_headers;
    bool m_sent_response_headers_to_client { false };

    AllocatingMemoryStream m_response_buffer;
    RefPtr<Core::Notifier> m_client_writer_notifier;
    Optional<RequestPipe> m_client_request_pipe;
    size_t m_bytes_transferred_to_client { 0 };

    Optional<Requests::NetworkError> m_network_error;
};

}
