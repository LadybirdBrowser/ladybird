/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/ThreadID.h>
#include <AK/Variant.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Engine/EngineEventSink.h>
#include <LibWeb/Fetch/Engine/FetchBodyDeliveryGate.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Engine/NetworkActivity.h>
#include <LibWeb/Fetch/Engine/NetworkActivityObservers.h>
#include <LibWeb/Fetch/Engine/NetworkTransportHandle.h>

namespace Web::Fetch::Engine {

class FetchEngineRecords;
class FetchExecutor;
class FetchLoad;

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

    // For a stop, abort or termination of the fetch: abandons the redirect continuation that is open, and any opened
    // later, and stops the fetch's body transport.
    void stop();

    void add_network_transport(NetworkTransportHandle&, FetchBodyDeliveryGate&);

    // Marks the channel as the body of a response handed over to the agent that started the fetch, with the id of that
    // delivery. A Closed channel, whose body has arrived whole, is not marked.
    void deliver_body(FetchByteChannel&);

    // Opens the continuation of a manual redirect the fetch hands over, which waits for the agent's answer. The
    // redirect's response arrived through the given network transport, if any.
    void open_redirect_continuation(NetworkTransportHandle*);
    [[nodiscard]] Optional<ContinuationId> open_redirect_continuation_id() const;

    enum class RedirectAnswerResult : u8 {
        Settled,
        // The continuation was settled before, or the id names another one.
        NotOpen,
        // Main fetch has not handed the redirect over yet.
        NotHandedOver,
    };
    // Settles the open continuation with the agent's answer once main fetch has handed the redirect over. Following also
    // lets main fetch run again.
    [[nodiscard]] RedirectAnswerResult resolve_redirect(ContinuationId, Command::ResolveRedirect::Resolution const&);

    // Starts a load of the fetch's request, which main fetch runs.
    [[nodiscard]] NonnullRefPtr<FetchLoad> start_load();

    // Main fetch hands a response over to the agent that started the fetch, unless main fetch has ended. Main fetch has
    // ended before the agent receives the delivery, and the fetch ends after it if nothing else is left.
    void main_fetch_hands_over(Event::ResponseDelivery);
    [[nodiscard]] bool main_fetch_has_ended() const { return m_main_fetch.has<MainFetchEnded>(); }
    // Main fetch can no longer hand a response over. Does nothing once main fetch has ended.
    void main_fetch_cannot_hand_over();

    // Defers main fetch's work through the engine's executor; work deferred once the engine is gone never runs.
    void defer(Function<void()>);

    // Main fetch waits for a preload's response, until the agent resolves the wait or abandons it.
    void main_fetch_waits_for_preload();
    void main_fetch_stops_waiting_for_preload();
    // Ends main fetch's wait for a preload's response, after which main fetch can no longer hand a response over. Returns
    // whether main fetch was waiting.
    [[nodiscard]] bool abandon_preload_wait();

    // Whether the fetch has ended: main fetch has ended, no load is live and no redirect continuation is open.
    [[nodiscard]] bool has_ended() const { return m_ended; }

private:
    friend class FetchEngineRecords;
    friend class FetchLoad;

    FetchEngineRecord(FetchEngineRecords&, FetchId, NonnullOwnPtr<EngineEventSink>, NonnullRefPtr<NetworkActivityObservers const>);

    [[nodiscard]] bool is_on_owner_thread() const { return m_owner_thread.is_current_thread(); }

    // Closes the open redirect continuation, releasing the lease of its network transport and stopping the body
    // transport.
    void close_redirect_continuation();
    void abandon_redirect();
    void did_end_load();
    void end_main_fetch();
    // Sends the fetch's end as its last event once nothing keeps the fetch from ending.
    void end_fetch_unless_anything_is_left();

    AK::ThreadID m_owner_thread { AK::ThreadID::current() };
    WeakPtr<FetchEngineRecords> m_records;
    FetchId m_fetch_id;
    NonnullOwnPtr<EngineEventSink> m_event_sink;
    NonnullRefPtr<NetworkActivityObservers const> m_network_activity_observers;
    WeakPtr<FetchBodyDeliveryGate::Transport> m_body_transport;
    WeakPtr<FetchBodyDeliveryGate::Transport> m_cors_preflight_body_transport;

    struct RedirectContinuation {
        ContinuationId id;
        WeakPtr<NetworkTransportHandle> network_transport;
    };
    Optional<RedirectContinuation> m_redirect_continuation;

    // Whether a stop, abort or termination of the fetch has reached the engine.
    bool m_stopped { false };

    struct MainFetchRunning { };
    struct MainFetchWaitsForPreload { };
    struct MainFetchEnded { };
    Variant<MainFetchRunning, MainFetchWaitsForPreload, MainFetchEnded> m_main_fetch { MainFetchRunning {} };
    size_t m_live_load_count { 0 };
    bool m_ended { false };
};

// One load of a fetch's request: from its start until its transport will deliver nothing more.
class WEB_API FetchLoad final : public RefCounted<FetchLoad> {
public:
    // The load has resolved its pending response, possibly with a network error.
    void did_give_response();
    // Ends the load, unless it has ended already.
    void end();
    // Ends the load at its last callback. Main fetch can no longer hand a response over if the load has not resolved its
    // pending response.
    void end_at_its_last_callback();
    [[nodiscard]] bool has_ended() const { return m_ended; }

private:
    friend class FetchEngineRecord;

    explicit FetchLoad(FetchEngineRecord&);

    NonnullRefPtr<FetchEngineRecord> m_record;
    bool m_gave_response { false };
    bool m_ended { false };
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

    // The executor that runs the engine, which outlives the records.
    void attach_to_executor(FetchExecutor&);
    void defer(Function<void()>);

    // Reports to the agent a fetch id no higher than that of an earlier start, and returns null for it.
    [[nodiscard]] RefPtr<FetchEngineRecord> start(FetchId, NonnullOwnPtr<EngineEventSink>, NonnullRefPtr<NetworkActivityObservers const>);

    // Handles a stop, abort or termination of the fetch. Does nothing once the fetch's record is gone. Reports to the
    // agent a fetch id above that of every started fetch.
    void stop(FetchId);

    // An answer for a continuation that is not open, or for a fetch whose record is gone, is dropped.
    struct RedirectAnswerDropped { };
    struct RedirectAnswerMisbehaved { };
    using RedirectAnswerOutcome = Variant<NonnullRefPtr<FetchEngineRecord>, RedirectAnswerDropped, RedirectAnswerMisbehaved>;

    // Settles the open continuation the answer names, returning the fetch's record. Reports to the agent a fetch id
    // above every start, a continuation id above every offer, or an answer before the redirect's handover.
    [[nodiscard]] RedirectAnswerOutcome resolve_redirect(Command::ResolveRedirect const&);
    void did_offer_redirect_continuation(Badge<FetchEngineRecord>, ContinuationId);

    // Does nothing once the fetch has ended or its record is gone. Reports to the agent a fetch id above that of every
    // started fetch, or a fetch whose main fetch does not wait for a preload.
    void abandon_preload_wait(FetchId);

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
    FetchExecutor* m_executor { nullptr };
    AK::ThreadID m_owner_thread { AK::ThreadID::current() };
    FetchId m_last_started_fetch_id { 0 };
    ContinuationId m_last_offered_continuation_id { 0 };
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
