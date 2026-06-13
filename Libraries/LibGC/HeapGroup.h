/*
 * Copyright (c) 2026, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Forward.h>

namespace GC {

// A group of heaps whose objects may reference each other through CrossHeapMember edges.
class GC_API HeapGroup {
    AK_MAKE_NONCOPYABLE(HeapGroup);
    AK_MAKE_NONMOVABLE(HeapGroup);

public:
    HeapGroup() = default;
    ~HeapGroup();

    void add(Heap&);
    void remove(Heap&);

    void collect_garbage(bool print_report = false);

private:
    Vector<Heap*> m_heaps;
};

}
