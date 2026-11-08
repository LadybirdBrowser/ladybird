/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>

namespace Web::Fetch::Fetching {

void FetchEngine::handle_command(Engine::EngineCommand command)
{
    command.visit(
        [](Engine::Command::StartFetch const& start_fetch) {
            run_deferred_main_fetch_steps(start_fetch.input.fetch_id);
        },
        [](Engine::Command::Abort const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::Terminate const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::Stop const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::ResolveRedirect const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::PreloadResponseResolved const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::TransportControl const&) { VERIFY_NOT_REACHED(); },
        [](Engine::Command::AdoptNetworkTransport const&) { VERIFY_NOT_REACHED(); });
}

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor()
{
    Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>());
}

}
