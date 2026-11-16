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
    m_body_transport = transport;
}

void FetchEngineRecord::stop_body_transport()
{
    if (auto transport = exchange(m_body_transport, nullptr).strong_ref())
        transport->stop();
}

void FetchEngineRecord::set_cors_preflight_body_transport(FetchBodyDeliveryGate::Transport& transport)
{
    m_cors_preflight_body_transport = transport;
}

void FetchEngineRecord::stop_cors_preflight_body_transport()
{
    if (auto transport = exchange(m_cors_preflight_body_transport, nullptr).strong_ref())
        transport->stop();
}

void FetchEngineRecord::add_network_transport(NetworkTransportHandle& handle, FetchBodyDeliveryGate& delivery_gate)
{
    if (auto* records = m_records.ptr())
        records->add_network_transport(handle, delivery_gate);
}

Optional<DeliveryId> FetchEngineRecord::deliver_body(FetchByteChannel& channel)
{
    DeliveryId delivery_id { s_next_delivery_id.fetch_add(1) };
    if (!channel.set_delivery_id_unless_closed(delivery_id))
        return {};
    return delivery_id;
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

void FetchEngineRecords::stop_body_transport(FetchId fetch_id)
{
    ASSERT(is_on_owner_thread());
    if (fetch_id > m_last_started_fetch_id) {
        m_agent->misbehaved("The agent stopped a fetch it has not started"sv);
        return;
    }
    if (auto record = m_records.get(fetch_id); record.has_value())
        (*record)->stop_body_transport();
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
