/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/ThreadID.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWeb/Fetch/Engine/NetworkActivityObservers.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>

namespace Web::Fetch::Engine {

class FetchEngineRecords;

// What the fetch engine keeps of a fetch it has started. Every method runs on the engine's loop, and so does the
// record's destruction.
class WEB_API FetchEngineRecord final : public RefCounted<FetchEngineRecord> {
public:
    ~FetchEngineRecord();

    [[nodiscard]] EngineEventSink& event_sink() { return *m_event_sink; }

    // Reports what happened to one of the fetch's RequestServer requests to the agent that started the fetch.
    void report_network_activity(RequestId request_id, Event::NetworkActivity::Activity activity)
    {
        m_event_sink->handle_event(Event::NetworkActivity { .request_id = request_id, .activity = move(activity) });
    }

    // Those of the agent that started the fetch.
    [[nodiscard]] NetworkActivityObservers const& network_activity_observers() const { return m_network_activity_observers; }

    // The transport of the response body the fetch receives, which stopping, aborting or terminating the fetch stops.
    void set_body_transport(FetchBodyDeliveryGate::Transport&);
    void stop_body_transport();

    // The transport of the response body a CORS preflight for the fetch receives, which stopping, aborting or
    // terminating the fetch leaves alone.
    void set_cors_preflight_body_transport(FetchBodyDeliveryGate::Transport&);
    void stop_cors_preflight_body_transport();

    void add_network_transport(NetworkTransportHandle&, FetchBodyDeliveryGate&);

    // Marks the channel as the body of a response handed over to the agent that started the fetch, and returns the id
    // of that delivery. A Closed channel, whose body has arrived whole, is not marked and gets none.
    Optional<DeliveryId> deliver_body(FetchByteChannel&);

private:
    friend class FetchEngineRecords;

    FetchEngineRecord(FetchEngineRecords&, FetchId, NonnullOwnPtr<EngineEventSink>, NonnullRefPtr<NetworkActivityObservers const>);

    WeakPtr<FetchEngineRecords> m_records;
    FetchId m_fetch_id;
    NonnullOwnPtr<EngineEventSink> m_event_sink;
    NonnullRefPtr<NetworkActivityObservers const> m_network_activity_observers;
    WeakPtr<FetchBodyDeliveryGate::Transport> m_body_transport;
    WeakPtr<FetchBodyDeliveryGate::Transport> m_cors_preflight_body_transport;
};

// The records of the fetches an engine has started, by fetch id, and the network transports it controls. A record stays
// only while something else holds it. Every method, and each record's destruction, runs on the loop the records were
// created on.
class WEB_API FetchEngineRecords : public Weakable<FetchEngineRecords> {
    AK_MAKE_NONCOPYABLE(FetchEngineRecords);
    AK_MAKE_NONMOVABLE(FetchEngineRecords);

public:
    // The agent whose commands start and stop the fetches.
    class WEB_API Agent {
    public:
        AK_ALLOC_WITH_KMALLOC;

        virtual ~Agent() = default;

        // Called for a command no honest agent could have posted.
        virtual void misbehaved(StringView reason);
    };

    explicit FetchEngineRecords(NonnullOwnPtr<Agent>);

    // Reports to the agent a fetch id no higher than that of an earlier start, and returns null for it.
    [[nodiscard]] RefPtr<FetchEngineRecord> start(FetchId, NonnullOwnPtr<EngineEventSink>, NonnullRefPtr<NetworkActivityObservers const>);

    // Does nothing once the fetch's record is gone. Reports to the agent a fetch id above that of every started fetch.
    void stop_body_transport(FetchId);

    // Records a network transport, which the agent's transport controls name by the id and transport epoch of its
    // request.
    void add_network_transport(NetworkTransportHandle&, FetchBodyDeliveryGate&);
    // Does nothing once the transport is gone. Reports to the agent a client the engine had no transport of, or a request
    // id above every one it had for that client.
    void control_network_transport(Command::TransportControl const&);

    void remove(Badge<FetchEngineRecord>, FetchId);

    // Adding a network transport forgets those that are gone once the records hold twice as many as the last sweep left,
    // and at least this many.
    static constexpr size_t smallest_network_transport_count_to_sweep_at = 32;

private:
    [[nodiscard]] bool is_on_owner_thread() const { return m_owner_thread.is_current_thread(); }

    void forget_network_transports_that_are_gone();

    NonnullOwnPtr<Agent> m_agent;
    AK::ThreadID m_owner_thread { AK::ThreadID::current() };
    FetchId m_last_started_fetch_id { 0 };
    HashMap<FetchId, FetchEngineRecord*> m_records;

    struct NetworkTransport {
        WeakPtr<NetworkTransportHandle> handle;
        NonnullRefPtr<FetchBodyDeliveryGate> delivery_gate;
    };
    // The network transports of the requests of one RequestServer client.
    struct ClientNetworkTransports {
        RequestId highest_request_id { 0 };
        HashMap<RequestId, NetworkTransport> transports;
    };
    HashMap<TransportEpoch, ClientNetworkTransports> m_network_transports;
    size_t m_network_transport_count { 0 };
    size_t m_network_transport_count_to_sweep_at { smallest_network_transport_count_to_sweep_at };
};

}
