/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <LibMedia/PipelineStatus.h>

namespace Media {

// Replacing the handler waits for a dispatch on another thread to finish; the mutex asserts on the same thread.
class SynchronizedWakeHandler {
public:
    void set(PipelineWakeHandler handler)
    {
        MutexLocker locker { m_mutex };
        m_handler = move(handler);
    }

    void dispatch()
    {
        MutexLocker locker { m_mutex };
        if (m_handler)
            m_handler();
    }

private:
    Mutex m_mutex;
    PipelineWakeHandler m_handler;
};

}
