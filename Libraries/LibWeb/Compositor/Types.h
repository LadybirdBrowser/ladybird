/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/DistinctNumeric.h>
#include <AK/Types.h>

namespace Web::Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CompositorContextId);

inline CompositorContextId allocate_compositor_context_id()
{
    static Atomic<u64> s_next_id { 1 };
    return CompositorContextId { s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) };
}

}
