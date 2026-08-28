/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Vector.h>

namespace Ladybird {

class WindowCloseCoordinator {
public:
    using Preflight = Function<void(Function<void(bool)>)>;

    bool start(Vector<Preflight>, Function<void()> on_approved, Function<void()> on_canceled);
    void cancel();

    bool is_running() const { return m_is_running; }

private:
    void run_next_preflight();
    void finish(bool approved);

    Vector<Preflight> m_preflights;
    Function<void()> m_on_approved;
    Function<void()> m_on_canceled;
    size_t m_next_preflight_index { 0 };
    u64 m_generation { 0 };
    bool m_is_running { false };
};

}
