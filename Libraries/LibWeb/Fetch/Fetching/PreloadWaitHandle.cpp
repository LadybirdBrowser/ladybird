/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/EngineCommand.h>
#include <LibWeb/Fetch/Fetching/PreloadWaitHandle.h>

namespace Web::Fetch::Fetching {

NonnullRefPtr<PreloadWaitHandle> PreloadWaitHandle::create()
{
    return adopt_ref(*new PreloadWaitHandle);
}

// NB: This can run as the heap sweeps the callback that holds the handle, so it touches no GC object and only posts.
PreloadWaitHandle::~PreloadWaitHandle()
{
    if (auto const* armed = m_state.get_pointer<Armed>())
        armed->fetch_executor->post(Engine::Command::PreloadWaitAbandoned { armed->fetch_id });
}

void PreloadWaitHandle::arm(Engine::FetchId fetch_id, Engine::FetchExecutor& fetch_executor)
{
    VERIFY(m_state.has<Unarmed>());
    m_state = Armed { fetch_id, fetch_executor };
}

void PreloadWaitHandle::disarm()
{
    m_state = Disarmed {};
}

}
