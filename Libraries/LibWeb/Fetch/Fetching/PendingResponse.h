/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

// Non-standard wrapper around a possibly pending Infrastructure::Response.
// This is needed to fit the asynchronous nature of ResourceLoader into the synchronous expectations
// of the Fetch spec - we run 'in parallel' as a deferred_invoke(), which is still on the main thread;
// therefore we use callbacks to run portions of the spec that require waiting for an HTTP load.
class WEB_API PendingResponse : public RefCounted<PendingResponse> {
public:
    using Callback = Function<void(NonnullRefPtr<Infrastructure::Response>)>;

    [[nodiscard]] static NonnullRefPtr<PendingResponse> create(Infrastructure::FetchParams const&);
    [[nodiscard]] static NonnullRefPtr<PendingResponse> create(Infrastructure::FetchParams const&, NonnullRefPtr<Infrastructure::Response>);
    ~PendingResponse();

    void when_loaded(Callback);
    void resolve(NonnullRefPtr<Infrastructure::Response>);
    bool is_resolved() const { return m_response != nullptr; }

private:
    PendingResponse(NonnullRefPtr<Engine::FetchEngineRecord>, RefPtr<Infrastructure::Response>);

    void run_callback();

    NonnullRefPtr<Engine::FetchEngineRecord> m_engine_record;
    Callback m_callback;
    RefPtr<Infrastructure::Response> m_response;
};

}

GC_UNTRACED_CAPTURE_WITH_ROOTED_CALLBACKS(Web::Fetch::Fetching::PendingResponse);
