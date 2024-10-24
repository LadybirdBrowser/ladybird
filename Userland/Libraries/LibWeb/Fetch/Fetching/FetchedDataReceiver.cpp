/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/HeapFunction.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Fetch/Fetching/FetchedDataReceiver.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Streams/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Fetch::Fetching {

JS_DEFINE_ALLOCATOR(FetchedDataReceiver);

FetchedDataReceiver::FetchedDataReceiver(JS::NonnullGCPtr<Infrastructure::FetchParams const> fetch_params, JS::NonnullGCPtr<Streams::ReadableStream> stream)
    : m_fetch_params(fetch_params)
    , m_stream(stream)
{
}

FetchedDataReceiver::~FetchedDataReceiver() = default;

void FetchedDataReceiver::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_params);
    visitor.visit(m_stream);
    visitor.visit(m_pending_promise);
}

void FetchedDataReceiver::set_pending_promise(JS::NonnullGCPtr<WebIDL::Promise> promise)
{
    auto had_pending_promise = m_pending_promise != nullptr;
    m_pending_promise = promise;

    if (!had_pending_promise && !m_buffer.is_empty()) {
        on_data_received(m_buffer);
        m_buffer.clear();
    }
}

// This implements the parallel steps of the pullAlgorithm in HTTP-network-fetch.
// https://fetch.spec.whatwg.org/#ref-for-in-parallel④
void FetchedDataReceiver::on_data_received(ReadonlyBytes bytes)
{
    // FIXME: 1. If the size of buffer is smaller than a lower limit chosen by the user agent and the ongoing fetch
    //           is suspended, resume the fetch.
    // FIXME: 2. Wait until buffer is not empty.

    // If the remote end sends data immediately after we receive headers, we will often get that data here before the
    // stream tasks have all been queued internally. Just hold onto that data.
    if (!m_pending_promise) {
        m_buffer.append(bytes);
        return;
    }

    // 3. Queue a fetch task to run the following steps, with fetchParams’s task destination.
    Infrastructure::queue_fetch_task(
        m_fetch_params->controller(),
        m_fetch_params->task_destination().get<JS::NonnullGCPtr<JS::Object>>(),
        JS::create_heap_function(heap(), [this, bytes = MUST(ByteBuffer::copy(bytes))]() mutable {
            HTML::TemporaryExecutionContext execution_context { m_stream->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // 1. Pull from bytes buffer into stream.
            if (auto result = Streams::readable_stream_pull_from_bytes(m_stream, move(bytes)); result.is_error()) {
                auto throw_completion = Bindings::dom_exception_to_throw_completion(m_stream->vm(), result.release_error());

                dbgln("FetchedDataReceiver: Stream error pulling bytes");
                HTML::report_exception(throw_completion, m_stream->realm());

                return;
            }

            // 2. If stream is errored, then terminate fetchParams’s controller.
            if (m_stream->is_errored())
                m_fetch_params->controller()->terminate();

            // 3. Resolve promise with undefined.
            WebIDL::resolve_promise(m_stream->realm(), *m_pending_promise, JS::js_undefined());
        }));
}

}
