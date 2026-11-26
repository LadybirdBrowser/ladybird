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
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/Loader/ResourceLoader.h>

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
        [this](Engine::Command::Abort const& abort) { m_records.stop(abort.fetch_id); },
        [this](Engine::Command::Terminate const& terminate) { m_records.stop(terminate.fetch_id); },
        [this](Engine::Command::Stop const& stop) { m_records.stop(stop.fetch_id); },
        [this](Engine::Command::ResolveRedirect const& resolve) {
            auto outcome = m_records.resolve_redirect(resolve);
            if (!resolve.resolution.has<Engine::Command::ResolveRedirect::Follow>())
                return;
            outcome.visit(
                [&](NonnullRefPtr<Engine::FetchEngineRecord>& record) { run_deferred_main_fetch_steps(resolve.fetch_id, move(record)); },
                [&](Engine::FetchEngineRecords::RedirectAnswerDropped) { drop_deferred_main_fetch_steps(resolve.fetch_id); },
                [](Engine::FetchEngineRecords::RedirectAnswerMisbehaved) { });
        },
        [this](Engine::Command::PreloadResponseResolved& resolved) { m_records.resolve_preload_wait(resolved.fetch_id, move(resolved.response)); },
        [this](Engine::Command::PreloadWaitAbandoned const& abandoned) { m_records.abandon_preload_wait(abandoned.fetch_id); },
        [this](Engine::Command::TransportControl const& control) { m_records.control_network_transport(control); },
        [this](Engine::Command::NavigationBodyAdopted const& adopted) { m_records.add_network_transport(*adopted.handle, *adopted.delivery_gate); },
        [](Engine::Command::AdoptNetworkTransport const&) { VERIFY_NOT_REACHED(); });
}

// Looks a host up through the page of the fetch's controller, on the agent that started the fetch.
class PageKnownHSTSHostProvider final : public Engine::KnownHSTSHostProvider {
public:
    virtual bool is_known_hsts_host(Infrastructure::FetchControllerToken controller_token, String const& host) override
    {
        auto controller = HTML::surrounding_agent().fetch_controller_registry.find(controller_token);
        VERIFY(controller);
        return ResourceLoader::is_known_hsts_host(controller->page(), host);
    }
};

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers)
{
    Engine::FetchExecutorServices services {
        .event_loop = Core::EventLoop::current_weak(),
        .known_hsts_host_provider = make_ref_counted<PageKnownHSTSHostProvider>(),
    };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>(move(network_activity_observers), make<Engine::FetchEngineRecords::Agent>()));
}

}
