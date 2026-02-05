/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/Promise.h>

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
    if (!m_buffer.is_empty())
        m_body->append_sniff_bytes(m_buffer);
}

void FetchedDataReceiver::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_params);
    visitor.visit(m_response);
    visitor.visit(m_body);
    visitor.visit(m_stream);
    visitor.visit(m_pending_promise);
}

void FetchedDataReceiver::set_pending_promise(GC::Ref<WebIDL::Promise> promise)
{
    VERIFY(!m_pending_promise);
    VERIFY(!m_has_unfulfilled_promise);
    m_pending_promise = promise;

    if (!buffer_is_eof()) {
        pull_bytes_into_stream();
    } else if (m_lifecycle_state == LifecycleState::ReadyToClose) {
        close_stream();
    }
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel⑤
void FetchedDataReceiver::handle_network_bytes(ReadonlyBytes bytes, NetworkState state)
{
    VERIFY(m_lifecycle_state == LifecycleState::Receiving);

    if (state == NetworkState::Complete) {
        VERIFY(bytes.is_empty());
        m_lifecycle_state = LifecycleState::CompletePending;
        // Mark sniff bytes as complete when the stream ends
        if (m_body)
            m_body->set_sniff_bytes_complete();
    }

    if (state == NetworkState::Ongoing) {
        m_buffer.append(bytes);
        // Capture bytes for MIME sniffing
        if (m_body)
            m_body->append_sniff_bytes(bytes);
    }

    if (!m_pending_promise) {
        if (m_lifecycle_state == LifecycleState::CompletePending && buffer_is_eof() && !m_has_unfulfilled_promise)
            m_lifecycle_state = LifecycleState::ReadyToClose;
        return;
    }

    // 1. If one or more bytes have been transmitted from response’s message body, then:
    if (!bytes.is_empty()) {
        // 1. Let bytes be the transmitted bytes.

        // FIXME: 2. Let codings be the result of extracting header list values given `Content-Encoding` and response’s header list.
        // FIXME: 3. Increase response’s body info’s encoded size by bytes’s length.
        // FIXME: 4. Set bytes to the result of handling content codings given codings and bytes.
        // FIXME: 5. Increase response’s body info’s decoded size by bytes’s length.
        // FIXME: 6. If bytes is failure, then terminate fetchParams’s controller.

        // 7. Append bytes to buffer.
        pull_bytes_into_stream();

        // FIXME: 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask the user agent
        //           to suspend the ongoing fetch.
        return;
    }
    // 2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is readable,
    //    then close stream, and abort these in-parallel steps.
    if (m_stream->is_readable()) {
        VERIFY(m_lifecycle_state == LifecycleState::CompletePending);
        close_stream();
    }
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel④
void FetchedDataReceiver::pull_bytes_into_stream()
{
    VERIFY(m_lifecycle_state == LifecycleState::Receiving || m_lifecycle_state == LifecycleState::CompletePending);

    // FIXME: 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the ongoing fetch
    //           is suspended, resume the fetch.

    // 2. Wait until buffer is not empty.
    // NB: It would be nice to avoid a copy here, but ReadableStream::pull_from_bytes currently requires an allocated
    //     ByteBuffer to create a JS::ArrayBuffer.
    auto bytes = copy_unpulled_bytes();
    VERIFY(!bytes.is_empty());

    // 3. Queue a fetch task to run the following steps, with fetchParams’s task destination.
    VERIFY(!m_has_unfulfilled_promise);
    m_has_unfulfilled_promise = true;

    Infrastructure::queue_fetch_task(
        m_fetch_params->controller(),
        m_fetch_params->task_destination(),
        GC::create_function(heap(), [this, bytes = move(bytes), pending_promise = m_pending_promise]() mutable {
            m_has_unfulfilled_promise = false;
            VERIFY(m_lifecycle_state == LifecycleState::Receiving || m_lifecycle_state == LifecycleState::CompletePending);

            HTML::TemporaryExecutionContext execution_context { m_stream->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Pull from bytes buffer into stream.
            if (auto result = m_stream->pull_from_bytes(move(bytes)); result.is_error()) {
                auto throw_completion = Bindings::exception_to_throw_completion(m_stream->vm(), result.release_error());

                dbgln("FetchedDataReceiver: Stream error pulling bytes");
                HTML::report_exception(throw_completion, m_stream->realm());

                return;
            }

            // 2. If stream is errored, then terminate fetchParams’s controller.
            if (m_stream->is_errored())
                m_fetch_params->controller()->terminate();

            // 3. Resolve promise with undefined.
            WebIDL::resolve_promise(m_stream->realm(), *pending_promise, JS::js_undefined());

            if (m_lifecycle_state == LifecycleState::CompletePending && buffer_is_eof())
                m_lifecycle_state = LifecycleState::ReadyToClose;
        }));

    m_pending_promise = {};
}

void FetchedDataReceiver::close_stream()
{
    VERIFY(m_has_unfulfilled_promise == 0);
    VERIFY(buffer_is_eof());

    WebIDL::resolve_promise(m_stream->realm(), *m_pending_promise, JS::js_undefined());
    m_pending_promise = {};
    m_lifecycle_state = LifecycleState::Closed;
    m_stream->close();

    if (m_http_cache) {
        auto request = m_fetch_params->request();

        if (m_response && request->cache_mode() != HTTP::CacheMode::NoStore)
            m_http_cache->finalize_entry(request->current_url(), request->method(), request->header_list(), m_response->status(), m_response->header_list(), move(m_buffer));

        m_http_cache.clear();
    }
}

ByteBuffer FetchedDataReceiver::copy_unpulled_bytes()
{
    auto bytes = MUST(m_buffer.slice(m_pulled_bytes, m_buffer.size() - m_pulled_bytes));
    m_pulled_bytes += bytes.size();

    return bytes;
}

}
