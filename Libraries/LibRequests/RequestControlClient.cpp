/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibRequests/RequestControlClient.h>

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

    m_pending_cache_size_estimations.clear();
    m_pending_clear_cache_requests.clear();

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

void RequestControlClient::retrieve_http_cookie(int client_id, u64 request_id, RequestServer::RequestType request_type, u64 cookie_request_id, URL::URL url, RequestServer::IsPrivate is_private)
{
    String cookie;

    if (on_retrieve_http_cookie)
        cookie = on_retrieve_http_cookie(url, is_private);

    async_retrieved_http_cookie(client_id, request_id, request_type, cookie_request_id, cookie);
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

}
