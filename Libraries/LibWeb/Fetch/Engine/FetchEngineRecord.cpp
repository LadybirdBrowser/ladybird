/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Format.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>

namespace Web::Fetch::Engine {

static Atomic<u64> s_next_delivery_id { 1 };
static Atomic<u64> s_next_continuation_id { 1 };

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
    if (m_redirect_continuation.has_value())
        abandon_redirect();
    stop_body_transport();
    end_main_fetch_if_its_read_cannot_end();
}

void FetchEngineRecord::add_network_transport(NetworkTransportHandle& handle, FetchBodyDeliveryGate& delivery_gate)
{
    ASSERT(is_on_owner_thread());
    if (auto* records = m_records.ptr())
        records->add_network_transport(handle, delivery_gate);
}

Optional<DeliveryId> FetchEngineRecord::deliver_body(FetchByteChannel& channel)
{
    ASSERT(is_on_owner_thread());
    DeliveryId delivery_id { s_next_delivery_id.fetch_add(1) };
    if (!channel.set_delivery_id_unless_closed(delivery_id))
        return {};
    return delivery_id;
}

ContinuationId FetchEngineRecord::open_redirect_continuation(NetworkTransportHandle* network_transport)
{
    ASSERT(is_on_owner_thread());
    VERIFY(!m_redirect_continuation.has_value());

    ContinuationId continuation_id { s_next_continuation_id.fetch_add(1) };
    if (auto* records = m_records.ptr())
        records->did_offer_redirect_continuation({}, continuation_id);
    m_redirect_continuation = RedirectContinuation { continuation_id, network_transport };
    if (m_stopped)
        abandon_redirect();
    return continuation_id;
}

FetchEngineRecord::RedirectAnswerResult FetchEngineRecord::resolve_redirect(ContinuationId continuation_id, Command::ResolveRedirect::Resolution const& resolution)
{
    ASSERT(is_on_owner_thread());
    if (!m_redirect_continuation.has_value() || m_redirect_continuation->id != continuation_id)
        return RedirectAnswerResult::NotOpen;
    if (!m_main_fetch.has<MainFetchEnded>())
        return RedirectAnswerResult::NotHandedOver;

    resolution.visit(
        [&](Command::ResolveRedirect::Follow) {
            m_redirect_continuation.clear();
            m_main_fetch = MainFetchRunning {};
        },
        [&](Command::ResolveRedirect::AcceptAsFinal) {
            m_redirect_continuation.clear();
            end_fetch_unless_anything_is_left();
        },
        [&](Command::ResolveRedirect::Abandon) { abandon_redirect(); });
    return RedirectAnswerResult::Settled;
}

void FetchEngineRecord::abandon_redirect()
{
    auto continuation = m_redirect_continuation.release_value();
    if (auto network_transport = continuation.network_transport.strong_ref())
        network_transport->release_transfer_lease();
    stop_body_transport();
    end_fetch_unless_anything_is_left();
}

NonnullRefPtr<FetchLoad> FetchEngineRecord::start_load()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    ++m_live_load_count;
    return adopt_ref(*new FetchLoad(*this));
}

void FetchEngineRecord::main_fetch_handed_over()
{
    ASSERT(is_on_owner_thread());
    if (m_main_fetch.has<MainFetchEnded>())
        return;
    VERIFY(m_main_fetch.has<MainFetchRunning>() || m_main_fetch.has<MainFetchReadsWholeBody>());
    end_main_fetch();
}

void FetchEngineRecord::main_fetch_cannot_hand_over()
{
    ASSERT(is_on_owner_thread());
    if (m_main_fetch.has<MainFetchEnded>())
        return;
    RefPtr<FetchByteChannel> channel_main_fetch_reads;
    if (auto const* reads_whole_body = m_main_fetch.get_pointer<MainFetchReadsWholeBody>())
        channel_main_fetch_reads = reads_whole_body->channel;
    end_main_fetch();
    if (channel_main_fetch_reads)
        (void)channel_main_fetch_reads->cancel();
}

void FetchEngineRecord::main_fetch_reads_whole_body(FetchByteChannel& channel)
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    m_main_fetch = MainFetchReadsWholeBody { channel };
    end_main_fetch_if_its_read_cannot_end();
}

void FetchEngineRecord::main_fetch_waits_for_preload()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchRunning>());
    m_main_fetch = MainFetchWaitsForPreload {};
}

void FetchEngineRecord::main_fetch_stops_waiting_for_preload()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_main_fetch.has<MainFetchWaitsForPreload>());
    m_main_fetch = MainFetchRunning {};
}

bool FetchEngineRecord::abandon_preload_wait()
{
    ASSERT(is_on_owner_thread());
    if (!m_main_fetch.has<MainFetchWaitsForPreload>())
        return false;
    end_main_fetch();
    return true;
}

void FetchEngineRecord::did_end_load()
{
    ASSERT(is_on_owner_thread());
    VERIFY(m_live_load_count > 0);
    --m_live_load_count;
    end_main_fetch_if_its_read_cannot_end();
    end_fetch_unless_anything_is_left();
}

void FetchEngineRecord::end_main_fetch_if_its_read_cannot_end()
{
    auto const* reads_whole_body = m_main_fetch.get_pointer<MainFetchReadsWholeBody>();
    if (reads_whole_body && !reads_whole_body->channel->can_be_read_to_its_end())
        end_main_fetch();
}

void FetchEngineRecord::end_main_fetch()
{
    m_main_fetch = MainFetchEnded {};
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

FetchEngineRecords::RedirectAnswerOutcome FetchEngineRecords::resolve_redirect(Command::ResolveRedirect const& resolve)
{
    ASSERT(is_on_owner_thread());
    if (resolve.fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent resolved a redirect of a fetch it has not started"sv);
        return RedirectAnswerMisbehaved {};
    }
    if (resolve.continuation_id > m_last_offered_continuation_id) {
        m_agent->misbehaved("The agent resolved a redirect the engine never offered"sv);
        return RedirectAnswerMisbehaved {};
    }

    auto found_record = m_records.get(resolve.fetch_id);
    if (!found_record.has_value())
        return RedirectAnswerDropped {};
    NonnullRefPtr record = **found_record;
    switch (record->resolve_redirect(resolve.continuation_id, resolve.resolution)) {
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

void FetchEngineRecords::did_offer_redirect_continuation(Badge<FetchEngineRecord>, ContinuationId continuation_id)
{
    ASSERT(is_on_owner_thread());
    m_last_offered_continuation_id = continuation_id;
}

void FetchEngineRecords::abandon_preload_wait(FetchId fetch_id)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent abandoned a preload wait of a fetch it has not started"sv);
        return;
    }

    auto found_record = m_records.get(fetch_id);
    if (!found_record.has_value())
        return;
    NonnullRefPtr record = **found_record;
    if (record->has_ended())
        return;
    if (!record->abandon_preload_wait())
        m_agent->misbehaved("The agent abandoned a preload wait of a fetch that waits for no preload"sv);
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
