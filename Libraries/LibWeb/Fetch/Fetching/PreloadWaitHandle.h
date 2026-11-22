/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Variant.h>
#include <AK/Weakable.h>
#include <LibGC/Forward.h>
#include <LibWeb/Fetch/Engine/FetchExecutor.h>
#include <LibWeb/Fetch/Engine/Ids.h>

namespace Web::Fetch::Fetching {

// Held by the callback that resolves a fetch's wait for a preloaded response. Destroyed while armed, it posts the
// abandonment of the wait to the fetch's engine.
class PreloadWaitHandle final
    : public RefCounted<PreloadWaitHandle>
    , public Weakable<PreloadWaitHandle> {
public:
    GC_HOLDS_NO_CELL_EDGES;

    [[nodiscard]] static NonnullRefPtr<PreloadWaitHandle> create();
    ~PreloadWaitHandle();

    // The fetch's main fetch waits from now on. A handle is armed at most once, and never after it is disarmed.
    void arm(Engine::FetchId, Engine::FetchExecutor&);
    // From now on the handle posts nothing.
    void disarm();

private:
    PreloadWaitHandle() = default;

    struct Unarmed { };
    struct Armed {
        Engine::FetchId fetch_id;
        NonnullRefPtr<Engine::FetchExecutor> fetch_executor;
    };
    struct Disarmed { };
    Variant<Unarmed, Armed, Disarmed> m_state { Unarmed {} };
};

}
