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
#include <AK/Weakable.h>
#include <LibCore/Proxy.h>
#include <LibDNS/Resolver.h>
#include <LibHTTP/HeaderMap.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/RequestTimingInfo.h>
#include <LibURL/URL.h>
#include <RequestServer/CacheLevel.h>
#include <RequestServer/Forward.h>
#include <RequestServer/RequestPipe.h>

struct curl_slist;

namespace RequestServer {

class Request : public Weakable<Request> {
public:
    static NonnullOwnPtr<Request> fetch(
        i32 request_id,
        Optional<DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    static NonnullOwnPtr<Request> connect(
        i32 request_id,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        CacheLevel cache_level);

    ~Request();

    URL::URL const& url() const { return m_url; }
    ByteString const& method() const { return m_method; }
    UnixDateTime request_start_time() const { return m_request_start_time; }

    void notify_request_unblocked(Badge<DiskCache>);
    void notify_fetch_complete(Badge<ConnectionFromClient>, int result_code);

private:
    enum class Type : u8 {
        Fetch,
        Connect,
    };

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

    Request(
        i32 request_id,
        Optional<DiskCache&> disk_cache,
        ConnectionFromClient& client,
        void* curl_multi,
        Resolver& resolver,
        URL::URL url,
        ByteString method,
        HTTP::HeaderMap request_headers,
        ByteBuffer request_body,
        ByteString alt_svc_cache_path,
        Core::ProxyData proxy_data);

    Request(
        i32 request_id,
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

    void transfer_headers_to_client_if_needed();
    ErrorOr<void> write_queued_bytes_without_blocking();

    u32 acquire_status_code() const;
    Requests::RequestTimingInfo acquire_timing_info() const;

    ConnectionFromClient& client();

    i32 m_request_id { 0 };
    Type m_type { Type::Fetch };
    State m_state { State::Init };

    Optional<DiskCache&> m_disk_cache;
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
    HTTP::HeaderMap m_request_headers;
    ByteBuffer m_request_body;

    ByteString m_alt_svc_cache_path;
    Core::ProxyData m_proxy_data;

    u32 m_status_code { 0 };
    Optional<String> m_reason_phrase;

    HTTP::HeaderMap m_response_headers;
    bool m_sent_response_headers_to_client { false };

    AllocatingMemoryStream m_response_buffer;
    RefPtr<Core::Notifier> m_client_writer_notifier;
    Optional<RequestPipe> m_client_request_pipe;

    Optional<size_t> m_start_offset_of_response_resumed_from_cache;
    size_t m_bytes_transferred_to_client { 0 };

    Optional<CacheEntryReader&> m_cache_entry_reader;
    Optional<CacheEntryWriter&> m_cache_entry_writer;

    Optional<Requests::NetworkError> m_network_error;
};

}
