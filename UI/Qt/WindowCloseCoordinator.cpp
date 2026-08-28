/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/WindowCloseCoordinator.h>

namespace Ladybird {

bool WindowCloseCoordinator::start(Vector<Preflight> preflights, Function<void()> on_approved, Function<void()> on_canceled)
{
    if (m_is_running)
        return false;

    m_preflights = move(preflights);
    m_on_approved = move(on_approved);
    m_on_canceled = move(on_canceled);
    m_next_preflight_index = 0;
    m_is_running = true;
    ++m_generation;
    run_next_preflight();
    return true;
}

void WindowCloseCoordinator::cancel()
{
    if (m_is_running)
        finish(false);
}

void WindowCloseCoordinator::run_next_preflight()
{
    if (m_next_preflight_index >= m_preflights.size()) {
        finish(true);
        return;
    }

    auto preflight = move(m_preflights[m_next_preflight_index++]);
    auto generation = m_generation;
    preflight([this, generation](bool approved) {
        if (!m_is_running || generation != m_generation)
            return;
        if (!approved) {
            finish(false);
            return;
        }
        run_next_preflight();
    });
}

void WindowCloseCoordinator::finish(bool approved)
{
    auto on_complete = approved ? move(m_on_approved) : move(m_on_canceled);
    m_is_running = false;
    ++m_generation;
    m_preflights.clear();
    m_on_approved = {};
    m_on_canceled = {};
    if (on_complete)
        on_complete();
}

}
