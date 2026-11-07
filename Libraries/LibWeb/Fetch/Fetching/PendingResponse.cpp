/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::Fetch::Fetching {

GC_DEFINE_ALLOCATOR(PendingResponse);

GC::Ref<PendingResponse> PendingResponse::create(GC::Ref<Infrastructure::FetchController> fetch_controller)
{
    return GC::Heap::the().allocate<PendingResponse>(fetch_controller);
}

GC::Ref<PendingResponse> PendingResponse::create(GC::Ref<Infrastructure::FetchController> fetch_controller, NonnullRefPtr<Infrastructure::Response> response)
{
    return GC::Heap::the().allocate<PendingResponse>(fetch_controller, move(response));
}

GC::Ref<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params)
{
    return create(fetch_params.controller());
}

GC::Ref<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params, NonnullRefPtr<Infrastructure::Response> response)
{
    return create(fetch_params.controller(), move(response));
}

PendingResponse::PendingResponse(GC::Ref<Infrastructure::FetchController> fetch_controller, RefPtr<Infrastructure::Response> response)
    : m_fetch_controller(fetch_controller)
    , m_response(move(response))
{
    m_fetch_controller->add_pending_response({}, *this);
}

void PendingResponse::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_callback);
    visitor.visit(m_fetch_controller);
}

void PendingResponse::when_loaded(Callback callback)
{
    VERIFY(!m_callback);
    m_callback = callback;
    if (m_response)
        run_callback();
}

void PendingResponse::resolve(NonnullRefPtr<Infrastructure::Response> response)
{
    VERIFY(!m_response);
    m_response = move(response);
    if (m_callback)
        run_callback();
}

void PendingResponse::run_callback()
{
    VERIFY(m_callback);
    VERIFY(m_response);
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(*this), [](PendingResponse& self) {
        VERIFY(self.m_callback);
        VERIFY(self.m_response);
        // NB: The callback runs only once, so let go of it as it runs. A pending response can live for as long as its
        //     fetch streams, and the callback would otherwise keep what it captured alive all that time.
        auto callback = exchange(self.m_callback, nullptr);
        callback->function()(*self.m_response);
        self.m_fetch_controller->remove_pending_response({}, self);
    }));
}

}
