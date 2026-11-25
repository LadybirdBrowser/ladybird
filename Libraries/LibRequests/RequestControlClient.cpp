/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibRequests/RequestControlClient.h>
#include <LibThreading/ThreadPool.h>

namespace Requests {

RequestControlClient::RequestControlClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<RequestServerControlClientEndpoint, RequestServerControlEndpoint>(*this, move(transport))
{
}

RequestControlClient::~RequestControlClient() = default;

void RequestControlClient::die()
{
    for (auto& [id, promise] : m_pending_cache_size_estimations)
        promise->reject(Error::from_string_literal("RequestServer process died"));
    for (auto& [id, promise] : m_pending_clear_cache_requests)
        promise->reject(Error::from_string_literal("RequestServer process died"));
    for (auto& [id, promise] : m_pending_cache_access_profile_requests)
        promise->reject(Error::from_string_literal("RequestServer process died"));

    m_pending_cache_size_estimations.clear();
    m_pending_clear_cache_requests.clear();
    m_pending_cache_access_profile_requests.clear();

    if (!m_pending_adopter_designations.is_empty()) {
        Core::deferred_invoke([pending_adopter_designations = move(m_pending_adopter_designations)] {
            for (auto& [id, promise] : pending_adopter_designations)
                promise->reject(Error::from_string_literal("RequestServer process died"));
        });
    }

    if (auto request_server_died_callback = move(on_request_server_died)) {
        Core::deferred_invoke([request_server_died_callback = move(request_server_died_callback)]() mutable {
            request_server_died_callback();
        });
    }
}

void RequestControlClient::network_usage(Vector<NetworkUsage> usage, u64 interval_microseconds)
{
    if (on_network_usage)
        on_network_usage(move(usage), interval_microseconds);
}

void RequestControlClient::rejected_certificate(ByteString host, u16 port, ByteString fingerprint, CertificateError errors, ByteString details)
{
    if (on_rejected_certificate)
        on_rejected_certificate(host, port, fingerprint, errors, details);
}

void RequestControlClient::client_disconnected(int client_id)
{
    if (on_client_disconnected)
        on_client_disconnected(client_id);
}

void RequestControlClient::retrieve_http_cookie(int client_id, u64 request_id, RequestServer::RequestType request_type, u64 cookie_request_id, URL::URL url, Optional<HTTP::Cookie::PartitionContext> partition_context, HTTP::Cookie::SameSiteContext same_site_context)
{
    String cookie;

    if (on_retrieve_http_cookie)
        cookie = on_retrieve_http_cookie(client_id, url, partition_context, same_site_context);

    async_retrieved_http_cookie(client_id, request_id, request_type, cookie_request_id, cookie);
}

// RequestServer gives up on a system lookup once it has waited this long for the answer, the same deadline that its
// DNS::Resolver sets for a system resolution.
static constexpr auto SYSTEM_LOOKUP_DEADLINE = AK::Duration::from_seconds(15);

void RequestControlClient::resolve_host_with_system_resolver(u64 resolution_id, ByteString host, bool ipv6)
{
    // getaddrinfo() blocks, for as long as the lookup takes, so it runs on a worker, and the answer goes back to
    // RequestServer from this thread.
    auto queued_at = MonotonicTime::now();
    Threading::ThreadPool::for_name_resolution().submit([weak_this = make_weak_ptr<RequestControlClient>(), resolution_id, host = move(host), ipv6, queued_at, event_loop = Core::EventLoop::current_weak()] mutable {
        // A lookup that waited in the queue past the deadline has no one left to answer, so it isn't made, and
        // RequestServer gets a failure for it, which it drops.
        Optional<Vector<ByteString>> addresses;
        if (MonotonicTime::now() - queued_at < SYSTEM_LOOKUP_DEADLINE) {
            auto family = ipv6 ? Core::Socket::AddressFamily::IPv6Only : Core::Socket::AddressFamily::IPv4Only;
            auto addresses_or_error = Core::Socket::resolve_host(host, Core::Socket::SocketType::Stream, family);
            if (!addresses_or_error.is_error()) {
                addresses = Vector<ByteString> {};
                for (auto const& address : addresses_or_error.value())
                    address.visit([&](auto const& address) { addresses->append(ByteString::formatted("{}", address)); });
            }
        }

        auto origin_event_loop = event_loop->take();
        if (!origin_event_loop.is_alive())
            return;
        origin_event_loop->deferred_invoke([weak_this = move(weak_this), resolution_id, addresses = move(addresses)] mutable {
            if (auto self = weak_this.strong_ref())
                self->async_resolved_host_with_system_resolver(resolution_id, move(addresses));
        });
    });
}

void RequestControlClient::store_response_cookies_and_hsts_policy(int client_id, u64 request_id, u64 store_request_id, URL::URL url, Optional<HTTP::Cookie::PartitionContext> partition_context, HTTP::Cookie::SameSiteContext same_site_context, Vector<HTTP::Cookie::ParsedCookie> cookies, Optional<HTTP::HSTS::ParsedHSTSPolicy> hsts_policy)
{
    if (on_store_response_cookies_and_hsts_policy)
        on_store_response_cookies_and_hsts_policy(client_id, url, partition_context, same_site_context, cookies, hsts_policy);

    async_stored_response_cookies_and_hsts_policy(client_id, request_id, store_request_id);
}

NonnullRefPtr<Core::Promise<CacheSizes>> RequestControlClient::estimate_cache_size_accessed_since(UnixDateTime since)
{
    auto promise = Core::Promise<CacheSizes>::construct();

    auto cache_size_estimation_id = m_next_cache_size_estimation_id++;
    m_pending_cache_size_estimations.set(cache_size_estimation_id, promise);

    async_estimate_cache_size_accessed_since(cache_size_estimation_id, since);

    return promise;
}

void RequestControlClient::estimated_cache_size(u64 cache_size_estimation_id, CacheSizes sizes)
{
    if (auto promise = m_pending_cache_size_estimations.take(cache_size_estimation_id); promise.has_value())
        (*promise)->resolve(sizes);
}

NonnullRefPtr<Core::Promise<Empty>> RequestControlClient::clear_cache(UnixDateTime since)
{
    auto promise = Core::Promise<Empty>::construct();

    auto clear_cache_request_id = m_next_clear_cache_request_id++;
    m_pending_clear_cache_requests.set(clear_cache_request_id, promise);

    async_remove_cache_entries_accessed_since(clear_cache_request_id, since);

    return promise;
}

void RequestControlClient::removed_cache_entries(u64 clear_cache_request_id)
{
    if (auto promise = m_pending_clear_cache_requests.take(clear_cache_request_id); promise.has_value())
        (*promise)->resolve({});
}

NonnullRefPtr<Core::Promise<Vector<CacheAccessBucket>>> RequestControlClient::retrieve_cache_access_profile(AK::Duration granularity)
{
    auto promise = Core::Promise<Vector<CacheAccessBucket>>::construct();

    auto access_profile_request_id = m_next_cache_access_profile_request_id++;
    m_pending_cache_access_profile_requests.set(access_profile_request_id, promise);

    async_retrieve_cache_access_profile(access_profile_request_id, granularity);

    return promise;
}

void RequestControlClient::retrieved_cache_access_profile(u64 access_profile_request_id, Vector<CacheAccessBucket> buckets)
{
    if (auto promise = m_pending_cache_access_profile_requests.take(access_profile_request_id); promise.has_value())
        (*promise)->resolve(move(buckets));
}

void RequestControlClient::disk_cache_size_changed(u64 size)
{
    if (on_disk_cache_size_changed)
        on_disk_cache_size_changed(size);
}

NonnullRefPtr<Core::Promise<Empty>> RequestControlClient::designate_request_adopter(RequestTransferLeaseKey transfer_lease, int adopter_client_id)
{
    auto promise = Core::Promise<Empty>::construct();
    if (!is_open()) {
        promise->reject(Error::from_string_literal("RequestServer process died"));
        return promise;
    }

    auto designation_id = m_next_adopter_designation_id++;
    m_pending_adopter_designations.set(designation_id, promise);

    async_designate_request_adopter(designation_id, transfer_lease.source_client_id, transfer_lease.source_request_id, adopter_client_id);

    return promise;
}

void RequestControlClient::request_adopter_designated(u64 designation_id)
{
    if (auto promise = m_pending_adopter_designations.take(designation_id); promise.has_value())
        (*promise)->resolve({});
}

}
