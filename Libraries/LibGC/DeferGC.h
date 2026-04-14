/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Heap.h>

namespace GC {

// note: write barriers remain active during deferral to ensure the tri-color
// invariant is preserved.
class GC_API DeferGC {
public:
    explicit DeferGC(Heap& heap)
        : m_heap(heap)
    {
        m_heap.defer_gc();
    }

    ~DeferGC()
    {
        m_heap.undefer_gc();
    }

private:
    Heap& m_heap;
};

}
