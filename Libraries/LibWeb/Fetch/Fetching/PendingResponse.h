/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>

namespace Web::Fetch::Fetching {

// Non-standard wrapper around a possibly pending Infrastructure::Response.
// This is needed to fit the asynchronous nature of ResourceLoader into the synchronous expectations
// of the Fetch spec - we run 'in parallel' as a deferred_invoke(), which is still on the main thread;
// therefore we use callbacks to run portions of the spec that require waiting for an HTTP load.
class PendingResponse : public JS::Cell {
    JS_CELL(PendingResponse, JS::Cell);
    JS_DECLARE_ALLOCATOR(PendingResponse);

public:
    using Callback = Function<void(JS::NonnullGCPtr<Infrastructure::Response>)>;

    [[nodiscard]] static JS::NonnullGCPtr<PendingResponse> create(JS::VM&, JS::NonnullGCPtr<Infrastructure::Request>);
    [[nodiscard]] static JS::NonnullGCPtr<PendingResponse> create(JS::VM&, JS::NonnullGCPtr<Infrastructure::Request>, JS::NonnullGCPtr<Infrastructure::Response>);

    void when_loaded(Callback);
    void resolve(JS::NonnullGCPtr<Infrastructure::Response>);
    bool is_resolved() const { return m_response != nullptr; }

private:
    PendingResponse(JS::NonnullGCPtr<Infrastructure::Request>, JS::GCPtr<Infrastructure::Response> = {});

    virtual void visit_edges(JS::Cell::Visitor&) override;

    void run_callback();

    JS::GCPtr<JS::HeapFunction<void(JS::NonnullGCPtr<Infrastructure::Response>)>> m_callback;
    JS::NonnullGCPtr<Infrastructure::Request> m_request;
    JS::GCPtr<Infrastructure::Response> m_response;
};

}
