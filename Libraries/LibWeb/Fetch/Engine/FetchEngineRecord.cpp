/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>

namespace Web::Fetch::Engine {

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

void FetchEngineRecords::remove(Badge<FetchEngineRecord>, FetchId fetch_id)
{
    ASSERT(is_on_owner_thread());
    m_records.remove(fetch_id);
}

}
