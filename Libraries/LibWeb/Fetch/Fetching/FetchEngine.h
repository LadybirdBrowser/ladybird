/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/EngineCommandSink.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>

namespace Web::Fetch::Fetching {

// Handles the commands posted for the fetches of the agent on whose thread it runs.
class FetchEngine final : public Engine::EngineCommandSink {
public:
    virtual void handle_command(Engine::EngineCommand) override;
};

// Creates an executor that runs a fetch engine on the current thread's event loop.
WEB_API NonnullRefPtr<Engine::FetchExecutor> create_inline_fetch_executor();

}
