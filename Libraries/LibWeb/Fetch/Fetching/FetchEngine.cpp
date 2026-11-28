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
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/HTML/Scripting/Agent.h>
#include <LibWeb/Loader/ResourceLoader.h>

namespace Web::Fetch::Fetching {

FetchEngine::FetchEngine(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers, NonnullOwnPtr<Engine::FetchEngineRecords::Agent> agent)
    : m_network_activity_observers(move(network_activity_observers))
    , m_records(move(agent))
{
}

static Optional<StringView> response_fault_of(Engine::MainFetchInput const& input)
{
    if (input.response && !input.response->is_network_error())
        return "The agent gave main fetch a response other than a network error"sv;
    auto const* candidate = input.fetch_params->preloaded_response_candidate().get_pointer<NonnullRefPtr<Infrastructure::Response>>();
    if (candidate && !(*candidate)->is_network_error()) {
        auto body = (*candidate)->unsafe_response()->body();
        if (!body.has_value() || !body->holds_all_its_bytes())
            return "The agent gave main fetch a preloaded response whose body is not all its bytes"sv;
    }
    return {};
}

void FetchEngine::handle_command(Engine::EngineCommand command)
{
    command.visit(
        [this](Engine::Command::StartFetch& start_fetch) {
            auto const& input = start_fetch.input.main_fetch;
            if (auto fault = response_fault_of(input); fault.has_value()) {
                m_records.agent_misbehaved(*fault);
                return;
            }
            if (input.fetch_params->engine_record()) {
                m_records.agent_misbehaved("The agent started a fetch whose fetch params have an engine record"sv);
                return;
            }
            auto record = m_records.start(start_fetch.input.fetch_id, move(start_fetch.input.event_sink), m_network_activity_observers);
            if (!record)
                return;
            run_main_fetch_parallel_steps(move(start_fetch.input.main_fetch), record.release_nonnull());
        },
        [this](Engine::Command::Abort const& abort) { m_records.stop(abort.fetch_id); },
        [this](Engine::Command::Terminate const& terminate) { m_records.stop(terminate.fetch_id); },
        [this](Engine::Command::Stop const& stop) { m_records.stop(stop.fetch_id); },
        [this](Engine::Command::ResolveRedirect& resolve) {
            auto* follow = resolve.resolution.get_pointer<Engine::Command::ResolveRedirect::Follow>();
            if (follow) {
                if (auto fault = response_fault_of(follow->next_hop); fault.has_value()) {
                    m_records.agent_misbehaved(*fault);
                    return;
                }
                auto const* engine_record = follow->next_hop.fetch_params->engine_record();
                if (!engine_record || engine_record->fetch_id() != resolve.fetch_id) {
                    m_records.agent_misbehaved("The agent followed a redirect with fetch params that do not run main fetch for that fetch"sv);
                    return;
                }
            }
            auto answer = resolve.resolution.visit(
                [](Engine::Command::ResolveRedirect::Follow const&) { return Engine::RedirectAnswer::Follow; },
                [](Engine::Command::ResolveRedirect::AcceptAsFinal) { return Engine::RedirectAnswer::AcceptAsFinal; },
                [](Engine::Command::ResolveRedirect::Abandon) { return Engine::RedirectAnswer::Abandon; });
            auto outcome = m_records.resolve_redirect(resolve.fetch_id, resolve.continuation_id, answer);
            auto* record = outcome.get_pointer<NonnullRefPtr<Engine::FetchEngineRecord>>();
            if (!follow || !record)
                return;
            if (follow->next_hop.fetch_params->control_block().is_ongoing())
                run_main_fetch_parallel_steps(move(follow->next_hop), *record);
            else
                (*record)->main_fetch_cannot_hand_over();
        },
        [this](Engine::Command::ResolveNetworkInterception& answer) { m_records.resolve_network_interception(move(answer)); },
        [this](Engine::Command::PreloadResponseResolved& resolved) { m_records.resolve_preload_wait(resolved.fetch_id, move(resolved.response)); },
        [this](Engine::Command::TransportControl const& control) { m_records.control_network_transport(control); },
        [this](Engine::Command::NavigationBodyAdopted const& adopted) { m_records.add_network_transport(*adopted.handle, *adopted.delivery_gate); },
        [](Engine::Command::AdoptNetworkTransport const&) { VERIFY_NOT_REACHED(); });
}

class ResourceLoaderKnownHSTSHostProvider final : public Engine::KnownHSTSHostProvider {
public:
    virtual Optional<bool> known_answer(String const& host) const override
    {
        if (ResourceLoader::is_preloaded_hsts_host(host))
            return true;
        if (!ResourceLoader::is_initialized())
            return false;
        return {};
    }

    virtual void look_up(Infrastructure::FetchParams const& fetch_params, String const& host, Function<void(bool is_known)> on_answer) override
    {
        auto network_isolation_key = Infrastructure::determine_the_network_partition_key(fetch_params.request());
        ResourceLoader::the().look_up_known_hsts_host(network_isolation_key, host, move(on_answer));
    }
};

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor(NonnullRefPtr<Engine::NetworkActivityObservers const> network_activity_observers)
{
    Engine::FetchExecutorServices services {
        .event_loop = Core::EventLoop::current_weak(),
        .known_hsts_host_provider = make_ref_counted<ResourceLoaderKnownHSTSHostProvider>(),
    };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>(move(network_activity_observers), make<Engine::FetchEngineRecords::Agent>()));
}

}
