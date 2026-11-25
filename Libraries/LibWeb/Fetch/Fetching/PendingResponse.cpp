/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>

namespace Web::Fetch::Fetching {

NonnullRefPtr<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params)
{
    VERIFY(fetch_params.engine_record());
    return adopt_ref(*new PendingResponse(*fetch_params.engine_record(), {}));
}

NonnullRefPtr<PendingResponse> PendingResponse::create(Infrastructure::FetchParams const& fetch_params, NonnullRefPtr<Infrastructure::Response> response)
{
    VERIFY(fetch_params.engine_record());
    return adopt_ref(*new PendingResponse(*fetch_params.engine_record(), move(response)));
}

PendingResponse::PendingResponse(NonnullRefPtr<Engine::FetchEngineRecord> engine_record, RefPtr<Infrastructure::Response> response)
    : m_engine_record(move(engine_record))
    , m_response(move(response))
{
}

PendingResponse::~PendingResponse() = default;

void PendingResponse::when_loaded(Callback callback)
{
    VERIFY(!m_callback);
    m_callback = move(callback);
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
    m_engine_record->defer([self = NonnullRefPtr { *this }] {
        VERIFY(self->m_callback);
        VERIFY(self->m_response);
        // NB: The callback runs only once, so let go of it as it runs. A pending response can live for as long as its
        //     fetch streams, and the callback would otherwise keep what it captured alive all that time.
        auto callback = move(self->m_callback);
        callback(*self->m_response);
    });
}

}
