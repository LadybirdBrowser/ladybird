/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>
#include <LibWeb/Fetch/Fetching/FetchEngine.h>

namespace Web::Fetch::Fetching {

void FetchEngine::handle_command(Engine::EngineCommand)
{
    VERIFY_NOT_REACHED();
}

NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor()
{
    Engine::FetchExecutorServices services { .event_loop = Core::EventLoop::current_weak() };
    return Engine::InlineFetchExecutor::create(move(services), make<FetchEngine>());
}

}
