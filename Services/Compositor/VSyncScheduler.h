/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>

namespace Compositor {

class VSyncScheduler {
    AK_MAKE_NONCOPYABLE(VSyncScheduler);
    AK_MAKE_NONMOVABLE(VSyncScheduler);

public:
    virtual ~VSyncScheduler() = default;

    virtual void schedule(double refresh_rate) = 0;
    virtual Optional<MonotonicTime> most_recent_tick_time(MonotonicTime now, double refresh_rate) = 0;

protected:
    VSyncScheduler() = default;
};

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64> display_id, Function<void(MonotonicTime)>&&);

}
