/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/RefCounted.h>

namespace Ladybird {

// A backing store is reusable only after both the view and the display server have stopped reading it.
class BackingStoreRelease final : public RefCounted<BackingStoreRelease> {
public:
    explicit BackingStoreRelease(Function<void()> release)
        : m_release(move(release))
    {
    }

    // False means this lease is already complete, so a later use of the same backing store needs its own release.
    bool view_finished_reading()
    {
        if (!m_release)
            return false;
        m_view_finished_reading = true;
        release_if_ready();
        return true;
    }

    void compositor_finished_reading()
    {
        m_compositor_finished_reading = true;
        release_if_ready();
    }

    bool is_released() const { return !m_release; }

private:
    void release_if_ready()
    {
        if (!m_view_finished_reading || !m_compositor_finished_reading || !m_release)
            return;
        auto release = move(m_release);
        release();
    }

    Function<void()> m_release;
    bool m_view_finished_reading { false };
    bool m_compositor_finished_reading { false };
};

}
