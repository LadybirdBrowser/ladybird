/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Fetch::Fetching {

JS_DEFINE_ALLOCATOR(PendingResponse);

JS::NonnullGCPtr<PendingResponse> PendingResponse::create(JS::VM& vm, JS::NonnullGCPtr<Infrastructure::Request> request)
{
    return vm.heap().allocate<PendingResponse>(request);
}

JS::NonnullGCPtr<PendingResponse> PendingResponse::create(JS::VM& vm, JS::NonnullGCPtr<Infrastructure::Request> request, JS::NonnullGCPtr<Infrastructure::Response> response)
{
    return vm.heap().allocate<PendingResponse>(request, response);
}

PendingResponse::PendingResponse(JS::NonnullGCPtr<Infrastructure::Request> request, JS::GCPtr<Infrastructure::Response> response)
    : m_request(request)
    , m_response(response)
{
    m_request->add_pending_response({}, *this);
}

void PendingResponse::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_request);
    visitor.visit(m_response);
}

void PendingResponse::when_loaded(Callback callback)
{
    VERIFY(!m_callback);
    m_callback = JS::create_heap_function(heap(), move(callback));
    if (m_response)
        run_callback();
}

void PendingResponse::resolve(JS::NonnullGCPtr<Infrastructure::Response> response)
{
    VERIFY(!m_response);
    m_response = response;
    if (m_callback)
        run_callback();
}

void PendingResponse::run_callback()
{
    VERIFY(m_callback);
    VERIFY(m_response);
    Platform::EventLoopPlugin::the().deferred_invoke(JS::create_heap_function(heap(), [this] {
        VERIFY(m_callback);
        VERIFY(m_response);
        m_callback->function()(*m_response);
        m_request->remove_pending_response({}, *this);
    }));
}

}
