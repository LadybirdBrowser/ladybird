/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>

namespace Web::Fetch::Fetching {

FetchEngine::FetchEngine(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers, NonnullOwnPtr<Engine::FetchEngineRecords::Agent> agent)
    : m_network_activity_observers(move(network_activity_observers))
    , m_records(move(agent))
{
}

void FetchEngine::handle_command(Engine::EngineCommand command)
{
    command.visit(
        [this](Engine::Command::StartFetch& start_fetch) {
            auto record = m_records.start(start_fetch.input.fetch_id, move(start_fetch.input.event_sink), m_network_activity_observers);
            if (!record)
                return;
            run_deferred_main_fetch_steps(start_fetch.input.fetch_id, record.release_nonnull());
        },
        [this](Engine::Command::Abort const& abort) { m_records.stop_body_transport(abort.fetch_id); },
        [this](Engine::Command::Terminate const& terminate) { m_records.stop_body_transport(terminate.fetch_id); },
        [this](Engine::Command::Stop const& stop) { m_records.stop_body_transport(stop.fetch_id); },
        [](Engine::Command::ResolveRedirect const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::PreloadResponseResolved const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::TransportControl const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::AdoptNetworkTransport const&) { VERIFY_NOT_REACHED(); });
}

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers)
{
    Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>(move(network_activity_observers), make<Engine::FetchEngineRecords::Agent>()));
}

}
