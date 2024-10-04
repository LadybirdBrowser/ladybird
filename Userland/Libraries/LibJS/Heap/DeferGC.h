/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Heap.h>

namespace JS {

class DeferGC {
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
