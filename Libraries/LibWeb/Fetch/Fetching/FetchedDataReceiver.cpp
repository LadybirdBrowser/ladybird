/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(FetchedDataReceiver);

FetchedDataReceiver::FetchedDataReceiver(GC::Ref<Infrastructure::FetchParams const> fetch_params, GC::Ref<Streams::ReadableStream> stream, RefPtr<HTTP::MemoryCache> http_cache)
    : m_fetch_params(fetch_params)
    , m_stream(stream)
    , m_http_cache(move(http_cache))
{
}

FetchedDataReceiver::~FetchedDataReceiver() = default;

void FetchedDataReceiver::set_body(GC::Ref<Fetch::Infrastructure::Body> body)
{
    m_body = body;
    // Flush any bytes that were buffered before the body was set
    if (!m_pre_body_sniff_buffer.is_empty()) {
        m_body->append_sniff_bytes(m_pre_body_sniff_buffer);
        m_pre_body_sniff_buffer.clear();
    }
    // If the stream already completed before the body was set,
    // we missed the set_sniff_bytes_complete() call in handle_network_bytes.
    if (m_network_complete)
        m_body->set_sniff_bytes_complete();
}

void FetchedDataReceiver::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_params);
    visitor.visit(m_response);
    visitor.visit(m_body);
    visitor.visit(m_stream);
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel⑤
void FetchedDataReceiver::handle_network_bytes(ReadonlyBytes bytes, NetworkState state)
{
    if (state == NetworkState::Complete) {
        VERIFY(bytes.is_empty());
        m_network_complete = true;
        // Mark sniff bytes as complete when the stream ends
        if (m_body)
            m_body->set_sniff_bytes_complete();

        // 2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is readable,
        //    then close stream, and abort these in-parallel steps.
        Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [this]() {
            close_stream();
        }));
        return;
    }

    if (state == NetworkState::Error)
        return;

    // 1. If one or more bytes have been transmitted from response’s message body, then:
    if (bytes.is_empty())
        return;

    // 1. Let bytes be the transmitted bytes.

    // FIXME: 2. Let codings be the result of extracting header list values given `Content-Encoding` and response’s header list.
    // FIXME: 3. Increase response’s body info’s encoded size by bytes’s length.
    // FIXME: 4. Set bytes to the result of handling content codings given codings and bytes.
    // FIXME: 5. Increase response’s body info’s decoded size by bytes’s length.
    // FIXME: 6. If bytes is failure, then terminate fetchParams’s controller.

    // Capture bytes for MIME sniffing
    if (m_body) {
        m_body->append_sniff_bytes(bytes);
    } else if (m_pre_body_sniff_buffer.size() < Infrastructure::MAX_SNIFF_BYTES) {
        auto space_remaining = Infrastructure::MAX_SNIFF_BYTES - m_pre_body_sniff_buffer.size();
        m_pre_body_sniff_buffer.append(bytes.slice(0, min(bytes.size(), space_remaining)));
    }

    if (m_http_cache)
        m_cache_buffer.append(bytes);

    // 7. Append bytes to buffer.
    enqueue_into_stream(bytes);

    // FIXME: 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask the user agent
    //           to suspend the ongoing fetch.
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel④
void FetchedDataReceiver::enqueue_into_stream(ReadonlyBytes bytes)
{
    // FIXME: 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the ongoing fetch
    //           is suspended, resume the fetch.

    if (!m_stream->is_readable())
        return;

    auto& realm = m_stream->realm();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. Pull from bytes buffer into stream.
    auto byte_buffer = MUST(ByteBuffer::copy(bytes));
    auto array_buffer = JS::ArrayBuffer::create(realm, move(byte_buffer));
    auto view = JS::Uint8Array::create(realm, array_buffer->byte_length(), *array_buffer);

    auto& controller = m_stream->controller()->get<GC::Ref<Streams::ReadableByteStreamController>>();

    if (auto result = Streams::readable_byte_stream_controller_enqueue(*controller, view); result.is_error()) {
        auto throw_completion = Bindings::exception_to_throw_completion(realm.vm(), result.release_error());
        // 2. If stream is errored, then terminate fetchParams’s controller.
        Streams::readable_byte_stream_controller_error(*controller, throw_completion.value());
        m_fetch_params->controller()->terminate();
    }
}

void FetchedDataReceiver::close_stream()
{
    if (m_http_cache) {
        auto request = m_fetch_params->request();
        if (m_stream->is_readable() && !m_fetch_params->is_canceled()
            && m_response && request->cache_mode() != HTTP::CacheMode::NoStore) {
            m_http_cache->finalize_entry(request->current_url(), request->method(), request->header_list(), m_response->status(), m_response->header_list(), move(m_cache_buffer));
        }

        m_http_cache.clear();
    }

    if (!m_stream->is_readable())
        return;

    HTML::TemporaryExecutionContext execution_context { m_stream->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    m_stream->close();
}

}
