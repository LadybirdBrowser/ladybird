/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestHeap.h"
#include <LibGC/Heap.h>

GC::Heap& test_gc_heap()
{
    // FIXME: The GC heap should become thread aware!
    thread_local GC::Heap heap(nullptr, [](auto&) {});
    return heap;
}
