/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Format.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>

namespace Web::Fetch::Engine {

static Atomic<u64> s_next_delivery_id { 1 };
static Atomic<u64> s_next_continuation_id { 1 };
static Atomic<u64> s_next_cors_preflight_id { 1 };

FetchEngineRecord::FetchEngineRecord(FetchEngineRecords& records, FetchId fetch_id, NonnullOwnPtr<EngineEventSink> event_sink, NonnullRefPtr<NetworkActivityObservers const> network_activity_observers)
    : m_records(records)
    , m_fetch_id(fetch_id)
    , m_event_sink(move(event_sink))
    , m_network_activity_observers(move(network_activity_observers))
{
}

FetchEngineRecord::~FetchEngineRecord()
{
    if (auto* records = m_records.ptr())
        records->remove({}, m_fetch_id);
}

void FetchEngineRecord::set_body_transport(FetchBodyDeliveryGate::Transport& transport)
{
    ASSERT(is_on_owner_thread());
    m_body_transport = transport;
}

void FetchEngineRecord::stop_body_transport()
{
    ASSERT(is_on_owner_thread());
    if (auto transport = exchange(m_body_transport, nullptr).strong_ref())
        transport->stop();
}

void FetchEngineRecord::set_cors_preflight_body_transport(FetchBodyDeliveryGate::Transport& transport)
{
    ASSERT(is_on_owner_thread());
    m_cors_preflight_body_transport = transport;
}

void FetchEngineRecord::stop_cors_preflight_body_transport()
{
    ASSERT(is_on_owner_thread());
    if (auto transport = exchange(m_cors_preflight_body_transport, nullptr).strong_ref())
        transport->stop();
}

void FetchEngineRecord::stop()
{
    ASSERT(is_on_owner_thread());
    m_stopped = true;
    // AD-HOC: A stop, abort or termination ends main fetch's wait for a preloaded response, which Fetch's wait ignores.
    //         The fetch then ends without a handover.
    if (m_main_fetch.has<MainFetchWaitsForPreload>())
        end_main_fetch();
    if (m_redirect_continuation.has_value())
        abandon_redirect();
    if (m_network_interception.has_value())
        m_network_interception.release_value().resume(FailFetch {});
    stop_body_transport();
}

static bool phase_takes(NetworkPhase phase, NetworkDecision const& decision)
{
    return decision.visit(
        [&](ModifyRequest const&) { return phase == NetworkPhase::BeforeRequestSent; },
        [&](ModifyResponse const&) { return phase != NetworkPhase::BeforeRequestSent; },
        [&](OneOf<Authenticate, CancelAuthentication> auto const&) { return phase == NetworkPhase::AuthRequired; },
        [](auto const&) { return true; });
}

bool FetchEngineRecord::can_wait_at_network_phase(NetworkPhase phase) const
{
    ASSERT(is_on_owner_thread());
    return !m_stopped && m_network_activity_observers->intercepts(phase);
}

void FetchEngineRecord::wait_at_network_phase(Event::FetchNetworkActivity activity, Function<void(NetworkDecision)> resume)
{
    ASSERT(is_on_owner_thread());
    auto phase = activity.phase();
    VERIFY(phase.has_value());
    VERIFY(!m_stopped);
    VERIFY(!m_network_interception.has_value());
    ContinuationId continuation_id { s_next_continuation_id.fetch_add(1) };
    if (auto* records = m_records.ptr())
        records->did_offer_continuation({}, continuation_id);
    m_network_interception = NetworkInterception { continuation_id, *phase, move(resume) };
    activity.offer_interception(continuation_id);
    report_network_activity(move(activity));
}

FetchEngineRecord::NetworkInterceptionAnswerResult FetchEngineRecord::resolve_network_interception(ContinuationId continuation_id, NetworkDecision decision)
{
    ASSERT(is_on_owner_thread());
    if (!m_network_interception.has_value() || m_network_interception->id != continuation_id)
        return NetworkInterceptionAnswerResult::NotOpen;
    if (!phase_takes(m_network_interception->phase, decision))
        return NetworkInterceptionAnswerResult::Invalid;

    m_network_interception.release_value().resume(move(decision));
    return NetworkInterceptionAnswerResult::Settled;
}

CorsPreflightId FetchEngineRecord::start_cors_preflight()
{
    ASSERT(is_on_owner_thread());
    return CorsPreflightId { s_next_cors_preflight_id.fetch_add(1) };
}

void FetchEngineRecord::add_network_transport(NetworkTransportHandle& handle, FetchBodyDeliveryGate& delivery_gate)
{
    ASSERT(is_on_owner_thread());
    if (auto* records = m_records.ptr())
        records->add_network_transport(handle, delivery_gate);
}

void FetchEngineRecord::deliver_body(FetchByteChannel& channel)
{
    ASSERT(is_on_owner_thread());
    channel.set_delivery_id_unless_closed(DeliveryId { s_next_delivery_id.fetch_add(1) });
}

void FetchEngineRecord::open_redirect_continuation(NetworkTransportHandle* network_transport)
{
    ASSERT(is_on_owner_thread());
    VERIFY(!m_redirect_continuation.has_value());

    ContinuationId continuation_id { s_next_continuation_id.fetch_add(1) };
    if (auto* records = m_records.ptr())
        records->did_offer_continuation({}, continuation_id);
    m_redirect_continuation = RedirectContinuation { continuation_id, network_transport };
    if (m_stopped)
        abandon_redirect();
}

FetchEngineRecord::RedirectAnswerResult FetchEngineRecord::resolve_redirect(ContinuationId continuation_id, RedirectAnswer answer)
{
    ASSERT(is_on_owner_thread());
    if (!m_redirect_continuation.has_value() || m_redirect_continuation->id != continuation_id)
        return RedirectAnswerResult::NotOpen;
    if (!m_main_fetch.has<MainFetchEnded>())
        return RedirectAnswerResult::NotHandedOver;

    switch (answer) {
    case RedirectAnswer::Follow:
        // NB: Main fetch runs again before the stop of the body transport can end the redirect's load.
        m_main_fetch = MainFetchRunning {};
        close_redirect_continuation();
        break;
    case RedirectAnswer::AcceptAsFinal:
        m_redirect_continuation.clear();
        end_fetch_unless_anything_is_left();
        break;
    case RedirectAnswer::Abandon:
        abandon_redirect();
        break;
    }
    return RedirectAnswerResult::Settled;
}

Optional<ContinuationId> FetchEngineRecord::open_redirect_continuation_id() const
{
    ASSERT(is_on_owner_thread());
    if (!m_redirect_continuation.has_value())
        return {};
    return m_redirect_continuation->id;
}

void FetchEngineRecord::close_redirect_continuation()
{
    auto continuation = m_redirect_continuation.release_value();
    if (auto network_transport = continuation.network_transport.strong_ref())
        network_transport->release_transfer_lease();
    stop_body_transport();
}

void FetchEngineRecord::abandon_redirect()
{
    close_redirect_continuation();
    end_fetch_unless_anything_is_left();
}

NonnullRefPtr<FetchLoad> FetchEngineRecord::start_load()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    ++m_live_load_count;
    return adopt_ref(*new FetchLoad(*this));
}

void FetchEngineRecord::main_fetch_hands_over(Event::ResponseDelivery delivery)
{
    ASSERT(is_on_owner_thread());
    if (m_main_fetch.has<MainFetchEnded>())
        return;
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    set_main_fetch_ended();
    m_event_sink->handle_event(move(delivery));
    end_fetch_unless_anything_is_left();
}

void FetchEngineRecord::main_fetch_cannot_hand_over()
{
    ASSERT(is_on_owner_thread());
    if (m_main_fetch.has<MainFetchEnded>())
        return;
    end_main_fetch();
}

KnownHSTSHostProvider& FetchEngineRecord::known_hsts_host_provider()
{
    ASSERT(is_on_owner_thread());
    auto* records = m_records.ptr();
    VERIFY(records);
    return records->known_hsts_host_provider();
}

void FetchEngineRecord::defer(Function<void()> work)
{
    ASSERT(is_on_owner_thread());
    if (auto* records = m_records.ptr())
        records->defer(move(work));
}

void FetchEngineRecord::expect_preload_resolution()
{
    ASSERT(is_on_owner_thread());
    if (m_preload_wait_resolution.has<NoPreloadWait>())
        m_preload_wait_resolution = PreloadWaitUnresolved {};
}

void FetchEngineRecord::wait_for_preload(Function<void(RefPtr<Infrastructure::Response>)> resume)
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    expect_preload_resolution();
    if (auto* resolved_early = m_preload_wait_resolution.get_pointer<PreloadWaitResolvedEarly>()) {
        auto response = move(resolved_early->response);
        m_preload_wait_resolution = PreloadWaitResolved {};
        resume(move(response));
        return;
    }
    m_main_fetch = MainFetchWaitsForPreload { move(resume) };
}

bool FetchEngineRecord::resolve_preload_wait(RefPtr<Infrastructure::Response> response)
{
    ASSERT(is_on_owner_thread());
    if (!m_preload_wait_resolution.has<PreloadWaitUnresolved>())
        return false;

    auto* waiting = m_main_fetch.get_pointer<MainFetchWaitsForPreload>();
    if (!waiting) {
        m_preload_wait_resolution = PreloadWaitResolvedEarly { move(response) };
        return true;
    }
    m_preload_wait_resolution = PreloadWaitResolved {};
    auto resume = move(waiting->resume);
    m_main_fetch = MainFetchRunning {};
    resume(move(response));
    return true;
}

void FetchEngineRecord::did_end_load()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_live_load_count > 0);
    --m_live_load_count;
    end_fetch_unless_anything_is_left();
}

void FetchEngineRecord::set_main_fetch_ended()
{
    m_main_fetch = MainFetchEnded {};
    if (m_preload_wait_resolution.has<PreloadWaitResolvedEarly>())
        m_preload_wait_resolution = PreloadWaitResolved {};
}

void FetchEngineRecord::end_main_fetch()
{
    set_main_fetch_ended();
    end_fetch_unless_anything_is_left();
}

void FetchEngineRecord::end_fetch_unless_anything_is_left()
{
    if (m_ended || !m_main_fetch.has<MainFetchEnded>() || m_live_load_count > 0 || m_redirect_continuation.has_value())
        return;
    m_ended = true;
    m_event_sink->handle_event(Event::FetchEnded {});
}

FetchLoad::FetchLoad(FetchEngineRecord& record)
    : m_record(record)
{
}

void FetchLoad::did_give_response()
{
    ASSERT(m_record->is_on_owner_thread());
    m_gave_response = true;
}

void FetchLoad::end()
{
    ASSERT(m_record->is_on_owner_thread());
    if (m_ended)
        return;
    m_ended = true;
    m_record->did_end_load();
}

void FetchLoad::end_at_its_last_callback()
{
    end();
    if (!m_gave_response)
        m_record->main_fetch_cannot_hand_over();
}

void FetchEngineRecords::Agent::misbehaved(StringView reason)
{
    dbgln("FetchEngineRecords: The agent misbehaved: {}", reason);
    VERIFY_NOT_REACHED();
}

FetchEngineRecords::FetchEngineRecords(NonnullOwnPtr<Agent> agent)
    : m_agent(move(agent))
{
}

FetchEngineRecords::~FetchEngineRecords()
{
    // NB: A continuation that waits for the agent can hold the last reference to its own record, so the records drop
    //     those continuations, which never run.
    Vector<NonnullRefPtr<FetchEngineRecord>> waiting_records;
    for (auto& [fetch_id, record] : m_records) {
        if (record->m_network_interception.has_value() || record->m_main_fetch.has<FetchEngineRecord::MainFetchWaitsForPreload>())
            waiting_records.append(*record);
    }
    for (auto& record : waiting_records) {
        record->m_network_interception.clear();
        if (auto* waiting_for_preload = record->m_main_fetch.get_pointer<FetchEngineRecord::MainFetchWaitsForPreload>())
            waiting_for_preload->resume = nullptr;
    }
}

void FetchEngineRecords::attach_to_executor(FetchExecutor& executor)
{
    VERIFY(!m_executor);
    m_executor = &executor;
}

KnownHSTSHostProvider& FetchEngineRecords::known_hsts_host_provider()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_executor);
    return m_executor->known_hsts_host_provider();
}

void FetchEngineRecords::defer(Function<void()> work)
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_executor);
    m_executor->defer({}, move(work));
}

RefPtr<FetchEngineRecord> FetchEngineRecords::start(FetchId fetch_id, NonnullOwnPtr<EngineEventSink> event_sink, NonnullRefPtr<NetworkActivityObservers const> network_activity_observers)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id <= m_last_started_fetch_id) {
        m_agent->misbehaved("The agent started a fetch out of fetch id order"sv);
        return {};
    }
    m_last_started_fetch_id = fetch_id;

    auto record = adopt_ref(*new FetchEngineRecord(*this, fetch_id, move(event_sink), move(network_activity_observers)));
    m_records.set(fetch_id, record.ptr());
    return record;
}

void FetchEngineRecords::stop(FetchId fetch_id)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent stopped a fetch it has not started"sv);
        return;
    }
    auto found_record = m_records.get(fetch_id);
    if (!found_record.has_value())
        return;
    NonnullRefPtr record = **found_record;
    record->stop();
}

FetchEngineRecords::RedirectAnswerOutcome FetchEngineRecords::resolve_redirect(FetchId fetch_id, ContinuationId continuation_id, RedirectAnswer answer)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent resolved a redirect of a fetch it has not started"sv);
        return RedirectAnswerMisbehaved {};
    }
    if (continuation_id > m_last_offered_continuation_id) {
        m_agent->misbehaved("The agent resolved a redirect the engine never offered"sv);
        return RedirectAnswerMisbehaved {};
    }

    auto found_record = m_records.get(fetch_id);
    if (!found_record.has_value())
        return RedirectAnswerDropped {};
    NonnullRefPtr record = **found_record;
    switch (record->resolve_redirect(continuation_id, answer)) {
    case FetchEngineRecord::RedirectAnswerResult::Settled:
        return record;
    case FetchEngineRecord::RedirectAnswerResult::NotOpen:
        return RedirectAnswerDropped {};
    case FetchEngineRecord::RedirectAnswerResult::NotHandedOver:
        m_agent->misbehaved("The agent answered a redirect before it was handed over"sv);
        return RedirectAnswerMisbehaved {};
    }
    VERIFY_NOT_REACHED();
}

void FetchEngineRecords::did_offer_continuation(Badge<FetchEngineRecord>, ContinuationId continuation_id)
{
    ASSERT(is_on_owner_thread());
    m_last_offered_continuation_id = continuation_id;
}

void FetchEngineRecords::resolve_network_interception(Command::ResolveNetworkInterception answer)
{
    ASSERT(is_on_owner_thread());
    if (answer.fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent answered a network interception of a fetch it has not started"sv);
        return;
    }
    if (answer.continuation_id > m_last_offered_continuation_id) {
        m_agent->misbehaved("The agent answered a network interception the engine never offered"sv);
        return;
    }

    auto found_record = m_records.get(answer.fetch_id);
    if (!found_record.has_value())
        return;
    NonnullRefPtr record = **found_record;
    switch (record->resolve_network_interception(answer.continuation_id, move(answer.decision))) {
    case FetchEngineRecord::NetworkInterceptionAnswerResult::Settled:
    case FetchEngineRecord::NetworkInterceptionAnswerResult::NotOpen:
        return;
    case FetchEngineRecord::NetworkInterceptionAnswerResult::Invalid:
        m_agent->misbehaved("The agent answered a network interception with a decision its phase does not take"sv);
        return;
    }
    VERIFY_NOT_REACHED();
}

void FetchEngineRecords::resolve_preload_wait(FetchId fetch_id, RefPtr<Infrastructure::Response> response)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent resolved a preload wait of a fetch it has not started"sv);
        return;
    }

    auto found_record = m_records.get(fetch_id);
    if (!found_record.has_value())
        return;
    if (response && !response->is_network_error()) {
        auto body = response->unsafe_response()->body();
        if (!body.has_value() || !body->holds_all_its_bytes()) {
            m_agent->misbehaved("The agent resolved a preload wait with a response whose body is not all its bytes"sv);
            return;
        }
    }

    NonnullRefPtr record = **found_record;
    if (record->main_fetch_has_ended())
        return;
    if (!record->has_preload_candidate()) {
        m_agent->misbehaved("The agent resolved a preload wait of a fetch whose main fetch waits for no preload"sv);
        return;
    }
    if (!record->resolve_preload_wait(move(response)))
        m_agent->misbehaved("The agent resolved a preload wait twice"sv);
}

void FetchEngineRecords::add_network_transport(NetworkTransportHandle& handle, FetchBodyDeliveryGate& delivery_gate)
{
    ASSERT(is_on_owner_thread());
    if (m_network_transport_count >= m_network_transport_count_to_sweep_at)
        forget_network_transports_that_are_gone();

    auto& client_transports = m_network_transports.ensure(handle.transport_epoch());
    client_transports.highest_request_id = max(client_transports.highest_request_id, handle.request_id());
    auto result = client_transports.transports.set(handle.request_id(), { handle, delivery_gate });
    if (result == HashSetResult::InsertedNewEntry)
        ++m_network_transport_count;
}

void FetchEngineRecords::forget_network_transports_that_are_gone()
{
    m_network_transport_count = 0;
    for (auto& [transport_epoch, client_transports] : m_network_transports) {
        client_transports.transports.remove_all_matching([](auto&, auto& transport) { return !transport.handle; });
        m_network_transport_count += client_transports.transports.size();
    }
    m_network_transport_count_to_sweep_at = max(smallest_network_transport_count_to_sweep_at, 2 * m_network_transport_count);
}

void FetchEngineRecords::control_network_transport(Command::TransportControl const& control)
{
    ASSERT(is_on_owner_thread());
    auto client_transports = m_network_transports.get(control.transport_epoch);
    if (!client_transports.has_value() || control.request_id > client_transports->highest_request_id) {
        m_agent->misbehaved("The agent controlled a network transport the engine never had"sv);
        return;
    }

    auto transport = client_transports->transports.get(control.request_id);
    if (!transport.has_value())
        return;
    auto handle = transport->handle.strong_ref();
    if (!handle)
        return;
    auto delivery_gate = transport->delivery_gate;

    control.operation.visit(
        [&](Command::TransportControl::StopTransport) { handle->stop(); },
        [&](Command::TransportControl::ReleaseNavigationHold) { delivery_gate->release_hold(); },
        [&](Command::TransportControl::ReleaseNavigationHoldUpTo const& release) { delivery_gate->release_hold_up_to(release.byte_count); });
}

void FetchEngineRecords::remove(Badge<FetchEngineRecord>, FetchId fetch_id)
{
    ASSERT(is_on_owner_thread());
    m_records.remove(fetch_id);
}

}
