/*
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibHTTP/Cache/MemoryCache.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/ReadableByteStreamController.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/ReadableStreamOperations.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(FetchedDataReceiver);

static constexpr size_t maximum_pending_bytes = 5 * MiB;

FetchedDataReceiver::FetchedDataReceiver(GC::Ref<Streams::ReadableStream> stream)
    : FetchedDataReceiver(nullptr, stream, {})
{
}

FetchedDataReceiver::FetchedDataReceiver(GC::Ptr<Infrastructure::FetchParams const> fetch_params, GC::Ref<Streams::ReadableStream> stream, RefPtr<HTTP::MemoryCache> http_cache)
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
    // we missed the set_sniff_bytes_complete() call in handle_network_data.
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
void FetchedDataReceiver::handle_network_data(JS::Realm& realm, Requests::ResponseData data, NetworkState state)
{
    if (state == NetworkState::Complete) {
        VERIFY(data.bytes().is_empty());
        m_network_complete = true;
        // Mark sniff bytes as complete when the stream ends
        if (m_body)
            m_body->set_sniff_bytes_complete();

        // 2. Otherwise, if the bytes transmission for response’s message body is done normally and stream is readable,
        //    then close stream, and abort these in-parallel steps.
        // NB: The close follows any pending bytes through the same task queue; see deliver_pending_bytes().
        queue_delivery_task(realm);
        return;
    }

    if (state == NetworkState::Error)
        return;

    // 1. If one or more bytes have been transmitted from response’s message body, then:
    auto bytes = data.bytes();
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
        if (auto const& immutable_bytes = data.immutable_bytes(); immutable_bytes.has_value() && immutable_bytes->is_file_backed() && m_body->source().has<Empty>() && bytes.size() == immutable_bytes->size())
            m_body->set_source(*immutable_bytes, static_cast<u64>(immutable_bytes->size()));
        m_body->append_sniff_bytes(bytes);
    } else if (m_pre_body_sniff_buffer.size() < Infrastructure::MAX_SNIFF_BYTES) {
        auto space_remaining = Infrastructure::MAX_SNIFF_BYTES - m_pre_body_sniff_buffer.size();
        m_pre_body_sniff_buffer.append(bytes.slice(0, min(bytes.size(), space_remaining)));
    }

    if (m_http_cache && !m_cache_body_replaces_network_buffer) {
        if (auto const& immutable_bytes = data.immutable_bytes(); immutable_bytes.has_value() && immutable_bytes->is_file_backed() && m_cache_buffer.is_empty() && !m_cache_body.has_value()) {
            m_cache_body = *immutable_bytes;
        } else {
            if (m_cache_body.has_value()) {
                m_cache_buffer.append(m_cache_body->bytes());
                m_cache_body.clear();
            }
            m_cache_buffer.append(bytes);
        }
    }

    // 7. Append bytes to buffer.
    m_pending_bytes.append(bytes);
    queue_delivery_task(realm);

    // 8. If the size of buffer is larger than an upper limit chosen by the user agent, ask the user agent to suspend
    //    the ongoing fetch.
    if (m_pending_bytes.size() >= maximum_pending_bytes && !m_paused_network_delivery) {
        if (auto request = m_network_request.strong_ref()) {
            request->set_body_delivery_paused(true);
            m_paused_network_delivery = true;
        }
    }
}

// AD-HOC: These tasks are queued without a document on purpose. A navigation reads its new document's body through
//         this receiver while the fetch's task destination is still the previous document's global, and tasks
//         queued on that document stop running once it is replaced.
static void queue_networking_task(GC::Ref<GC::Function<void()>> steps)
{
    HTML::queue_a_task(HTML::Task::Source::Networking, HTML::main_thread_event_loop(), nullptr, steps);
}

// This implements the task-queueing half of the pullAlgorithm in HTTP-network-fetch: bytes are handed to the
// stream from a networking task rather than from the network callback, so consumers see them interleaved with
// the tasks their own reactions queue.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel④
void FetchedDataReceiver::queue_delivery_task(JS::Realm& realm)
{
    if (m_delivery_task_queued)
        return;
    m_delivery_task_queued = true;

    queue_networking_task(GC::create_function(heap(), [this, &realm]() {
        deliver_pending_bytes(realm);
    }));
}

void FetchedDataReceiver::deliver_pending_bytes(JS::Realm& realm)
{
    m_delivery_task_queued = false;

    auto bytes = move(m_pending_bytes);
    m_pending_bytes = {};
    if (!bytes.is_empty())
        enqueue_into_stream(realm, bytes);

    // 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the ongoing fetch is
    //    suspended, resume the fetch.
    if (m_paused_network_delivery) {
        m_paused_network_delivery = false;
        if (auto request = m_network_request.strong_ref())
            request->set_body_delivery_paused(false);
    }

    // The close goes through the queue too, so it lands behind whatever the consumer queued in reaction to the
    // bytes above, the way it does when a network completion trails the last data.
    if (m_network_complete) {
        queue_networking_task(GC::create_function(heap(), [this, &realm]() {
            close_stream(realm);
        }));
    }
}

void FetchedDataReceiver::set_cached_response_body(Core::ImmutableBytes body)
{
    if (!m_http_cache)
        return;

    m_cache_buffer.clear();
    m_cache_body = move(body);
    m_cache_body_replaces_network_buffer = true;
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel④
void FetchedDataReceiver::enqueue_into_stream(JS::Realm& realm, ReadonlyBytes bytes)
{
    if (!m_stream->is_readable())
        return;

    auto& controller = m_stream->controller()->get<GC::Ref<Streams::ReadableByteStreamController>>();
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // 1. Pull from bytes buffer into stream.
    auto byte_buffer = MUST(ByteBuffer::copy(bytes));

    if (auto result = Streams::readable_byte_stream_controller_enqueue_native_bytes(realm, *controller, move(byte_buffer)); result.is_error()) {
        auto throw_completion = WebIDL::exception_to_throw_completion(realm.vm(), realm, result.release_error());
        // 2. If stream is errored, then terminate fetchParams’s controller.
        Streams::readable_byte_stream_controller_error(*controller, throw_completion.value());
        if (m_fetch_params)
            m_fetch_params->controller()->terminate();
    }
}

void FetchedDataReceiver::close_stream(JS::Realm& realm)
{
    if (m_http_cache && m_fetch_params) {
        auto request = m_fetch_params->request();
        if (m_stream->is_readable() && !m_fetch_params->is_canceled()
            && m_response && request->cache_mode() != HTTP::CacheMode::NoStore) {
            auto response_body = m_cache_body.has_value()
                ? m_cache_body.release_value()
                : Core::ImmutableBytes::adopt(move(m_cache_buffer));
            m_http_cache->finalize_entry(request->current_url(), request->method(), request->header_list(), m_response->status(), m_response->header_list(), move(response_body));
        }

        m_http_cache.clear();
        m_cache_body.clear();
    }

    if (!m_stream->is_readable())
        return;

    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    m_stream->close();
}

}
