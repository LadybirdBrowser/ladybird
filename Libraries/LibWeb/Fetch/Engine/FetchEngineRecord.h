/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWeb/Fetch/Engine/NetworkActivityObservers.h>

namespace Web::Fetch::Engine {

// What the fetch engine keeps of a fetch it has started.
class FetchEngineRecord final : public RefCounted<FetchEngineRecord> {
public:
    [[nodiscard]] static NonnullRefPtr<FetchEngineRecord> create(NonnullOwnPtr<EngineEventSink> event_sink, NonnullRefPtr<NetworkActivityObservers const> network_activity_observers)
    {
        return adopt_ref(*new FetchEngineRecord(move(event_sink), move(network_activity_observers)));
    }

    [[nodiscard]] EngineEventSink& event_sink() { return *m_event_sink; }

    // Reports what happened to one of the fetch's RequestServer requests to the agent that started the fetch.
    void report_network_activity(RequestId request_id, Event::NetworkActivity::Activity activity)
    {
        m_event_sink->handle_event(Event::NetworkActivity { .request_id = request_id, .activity = move(activity) });
    }

    // Those of the agent that started the fetch.
    [[nodiscard]] NetworkActivityObservers const& network_activity_observers() const { return m_network_activity_observers; }

private:
    FetchEngineRecord(NonnullOwnPtr<EngineEventSink> event_sink, NonnullRefPtr<NetworkActivityObservers const> network_activity_observers)
        : m_event_sink(move(event_sink))
        , m_network_activity_observers(move(network_activity_observers))
    {
    }

    NonnullOwnPtr<EngineEventSink> m_event_sink;
    NonnullRefPtr<NetworkActivityObservers const> m_network_activity_observers;
};

}
