/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibHTTP/Cache/MemoryCache.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Fetching/NetworkBodyPump.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/WebDriver/BiDi/Network.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(NetworkBodyPump);

// https://fetch.spec.whatwg.org/#concept-http-network-fetch
// NB: Step 9 of HTTP-network fetch. Steps 10 to 13 run in create_body_from_channel(), when the body's view is made.
GC::Ref<NetworkBodyPump> NetworkBodyPump::create(RefPtr<Infrastructure::FetchParams const> fetch_params, Engine::FetchBodyDeliveryGate::NavigationHold navigation_hold, RefPtr<HTTP::MemoryCache> http_cache)
{
    // 9. Let buffer be an empty byte sequence.
    // NB: The buffer is the FetchByteChannel; its watermarks are the user-agent-chosen lower and upper limits.
    static constexpr Engine::FetchByteChannel::Watermarks network_body_watermarks { .low = 256 * KiB, .high = 1 * MiB };
    auto delivery_gate = Engine::FetchBodyDeliveryGate::create(navigation_hold);
    auto channel = Engine::FetchByteChannel::create(network_body_watermarks, delivery_gate->create_channel_sink());
    delivery_gate->grant_credit(channel->initial_credit());

    return GC::Heap::the().allocate<NetworkBodyPump>(move(fetch_params), move(channel), move(delivery_gate), move(http_cache));
}

NetworkBodyPump::NetworkBodyPump(RefPtr<Infrastructure::FetchParams const> fetch_params, NonnullRefPtr<Engine::FetchByteChannel> channel, NonnullRefPtr<Engine::FetchBodyDeliveryGate> delivery_gate, RefPtr<HTTP::MemoryCache> http_cache)
    : m_fetch_params(move(fetch_params))
    , m_fetch_controller(m_fetch_params ? m_fetch_params->controller().ptr() : nullptr)
    , m_channel(move(channel))
    , m_delivery_gate(move(delivery_gate))
    , m_http_cache(move(http_cache))
{
}

NetworkBodyPump::~NetworkBodyPump() = default;

void NetworkBodyPump::set_response(NonnullRefPtr<Infrastructure::Response> response)
{
    m_response = move(response);
    if (m_network_complete)
        update_response_body_info();
}

void NetworkBodyPump::update_response_body_info()
{
    auto body_info = m_response->body_info();
    body_info.encoded_size = m_encoded_body_size;
    body_info.decoded_size = m_decoded_body_size;
    m_response->set_body_info(move(body_info));
}

void NetworkBodyPump::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_controller);
}

// This implements the transmission steps of HTTP-network fetch.
// https://fetch.spec.whatwg.org/#concept-http-network-fetch
void NetworkBodyPump::handle_network_data(Requests::ResponseData data)
{
    // 1. Run these steps, but abort when fetchParams is canceled:
    // AD-HOC: A stopped fetch is not canceled, but takes no more of its body either.
    if (m_fetch_params && !m_fetch_params->control_block().is_ongoing()) {
        m_channel->cancel();
        return;
    }

    // 1. If one or more bytes have been transmitted from response’s message body, then:
    auto bytes = data.bytes();
    if (bytes.is_empty())
        return;

    // 1. Let bytes be the transmitted bytes.

    // FIXME: 2. Let codings be the result of extracting header list values given `Content-Encoding` and response’s header list.
    // FIXME: 3. Let filteredCoding be "@unknown".
    // FIXME: 4. If codings is null or failure, then set filteredCoding to the empty string.
    // FIXME: 5. Otherwise, if codings’s size is greater than 1, then set filteredCoding to "multiple".
    // FIXME: 6. Otherwise, if codings[0] is the empty string, or it is supported by the user agent, and is a
    //           byte-case-insensitive match for an entry listed in the HTTP Content Coding Registry, then set
    //           filteredCoding to the result of byte-lowercasing codings[0].
    // FIXME: 7. Set response’s body info’s content encoding to filteredCoding.
    // FIXME: 8. Increase response’s body info’s encoded size by bytes’s length.
    // FIXME: 9. Set bytes to the result of handling content codings given codings and bytes.
    // FIXME: 11. If bytes is failure, then terminate fetchParams’s controller.

    if (m_delivery_gate->did_deliver(bytes.size()) == Engine::FetchBodyDeliveryGate::DeliveryResult::Rejected)
        return;

    // 10. Increase response’s body info’s decoded size by bytes’s length.
    // NB: The response's body info is updated once the network completes.
    m_decoded_body_size += bytes.size();

    // 12. Append bytes to buffer. (Steps 12 and 13 live in FetchByteChannel's writes.)
    auto const* file_backed = data.file_backed_slice();
    if (file_backed) {
        m_channel->write(file_backed->slice);
        m_channel->keep_whole_body(file_backed->payload);
    } else {
        m_channel->write_copy(bytes);
    }

    if (m_http_cache && !m_cache_body_replaces_network_buffer && !m_cache_body_too_large_for_memory) {
        if (file_backed && m_cache_buffer.is_empty()) {
            if (!m_cache_body.has_value())
                m_cache_body = file_backed->payload;
        } else if (!http_memory_cache_can_hold_body_in_memory(m_decoded_body_size)) {
            // NB: A mapped file of the whole body may still arrive, which the cache can hold.
            m_cache_body_too_large_for_memory = true;
            m_cache_buffer.clear();
            m_cache_body.clear();
        } else {
            if (m_cache_body.has_value()) {
                m_cache_buffer.append(m_cache_body->bytes());
                m_cache_body.clear();
            }
            m_cache_buffer.append(bytes);
        }
    }
}

void NetworkBodyPump::handle_network_complete(Optional<i64> request_server_encoded_body_size)
{
    m_delivery_gate->detach_transport();
    m_network_complete = true;
    // AD-HOC: RequestServer reports the encoded size of an HTTP body once it has been received. Other schemes have no
    //         content coding, so their encoded size is their decoded size.
    if (request_server_encoded_body_size.has_value())
        m_encoded_body_size = static_cast<u64>(max<i64>(*request_server_encoded_body_size, 0));
    else
        m_encoded_body_size = m_decoded_body_size;
    if (m_response)
        update_response_body_info();
    finalize_cache_entry();
    m_channel->close();
    if (m_fetch_controller)
        m_fetch_controller->did_complete_network_body(*m_channel);
}

void NetworkBodyPump::handle_network_error(StringView error_message)
{
    m_delivery_gate->detach_transport();
    if (WebDriver::BiDi::network_events_enabled() && m_fetch_params)
        WebDriver::BiDi::network_fetch_error(*m_fetch_params, error_message);
    abandon_cache_entry(HTTP::MemoryCache::StoredResponse::Keep);
    m_channel->error({ .message = MUST(ByteBuffer::copy(error_message.bytes())) });
}

void NetworkBodyPump::handle_network_teardown(Requests::Request::TeardownReason reason)
{
    m_delivery_gate->detach_transport();
    switch (reason) {
    case Requests::Request::TeardownReason::Stopped:
        (void)m_channel->cancel();
        break;
    case Requests::Request::TeardownReason::Transferred:
        (void)m_channel->transfer_out();
        break;
    }
    abandon_cache_entry(HTTP::MemoryCache::StoredResponse::Keep);
}

void NetworkBodyPump::set_cached_response_body(Core::ImmutableBytes body)
{
    if (!m_http_cache)
        return;

    m_cache_buffer.clear();
    m_cache_body = move(body);
    m_cache_body_replaces_network_buffer = true;
    m_cache_body_too_large_for_memory = false;
}

void NetworkBodyPump::finalize_cache_entry()
{
    if (!m_http_cache || !m_fetch_params)
        return;

    auto request = m_fetch_params->request();
    if (m_fetch_params->is_canceled() || !m_response || request->cache_mode() == HTTP::CacheMode::NoStore) {
        abandon_cache_entry(HTTP::MemoryCache::StoredResponse::Keep);
        return;
    }

    // NB: The cache cannot hold this response, which still replaces the one it has stored.
    if (m_cache_body_too_large_for_memory) {
        abandon_cache_entry(HTTP::MemoryCache::StoredResponse::Retire);
        return;
    }

    auto response_body = m_cache_body.has_value()
        ? m_cache_body.release_value()
        : Core::ImmutableBytes::adopt(move(m_cache_buffer));
    m_http_cache->finalize_entry(request->current_url(), request->method(), request->header_list(), m_response->status(), m_response->header_list(), move(response_body));
    evict_http_memory_cache_entries_over_limits();

    m_http_cache.clear();
    m_cache_body.clear();
}

void NetworkBodyPump::abandon_cache_entry(HTTP::MemoryCache::StoredResponse stored_response)
{
    if (m_http_cache && m_fetch_params && m_response) {
        auto request = m_fetch_params->request();
        m_http_cache->abandon_entry(request->current_url(), request->method(), request->header_list(), m_response->header_list(), stored_response);
    }

    m_http_cache.clear();
    m_cache_buffer.clear();
    m_cache_body.clear();
}

}
