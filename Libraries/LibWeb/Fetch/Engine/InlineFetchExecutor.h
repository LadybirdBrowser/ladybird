/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>

namespace Web::Fetch::Engine {

// Runs the engine as deferred work of the loop its services name.
class WEB_API InlineFetchExecutor final : public FetchExecutor {
public:
    static NonnullRefPtr<InlineFetchExecutor> create(FetchExecutorServices, NonnullOwnPtr<EngineCommandSink>);

private:
    InlineFetchExecutor(FetchExecutorServices, NonnullOwnPtr<EngineCommandSink>);

    virtual void post_task(Function<void()>) override;
};

}
