/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/InlineFetchExecutor.h>

namespace Web::Fetch::Engine {

NonnullRefPtr<InlineFetchExecutor> InlineFetchExecutor::create(FetchExecutorServices services, NonnullOwnPtr<EngineCommandSink> engine)
{
    return adopt_ref(*new InlineFetchExecutor(move(services), move(engine)));
}

InlineFetchExecutor::InlineFetchExecutor(FetchExecutorServices services, NonnullOwnPtr<EngineCommandSink> engine)
    : FetchExecutor(move(services), move(engine))
{
}

void InlineFetchExecutor::post_task(Function<void()> task)
{
    auto loop = services().event_loop->take();
    if (!loop)
        return;
    loop->deferred_invoke(move(task));
}

}
