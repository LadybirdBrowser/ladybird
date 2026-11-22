/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/kmalloc.h>
#include <LibWeb/Fetch/Engine/EngineCommand.h>

namespace Web::Fetch::Engine {

class FetchExecutor;

// Receives every posted command on the engine's thread, in posting order.
class EngineCommandSink {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~EngineCommandSink() = default;

    // Called once, by the executor that runs the engine and outlives it, before any command.
    virtual void attach_to_executor(FetchExecutor&) = 0;

    virtual void handle_command(EngineCommand) = 0;
};

}
