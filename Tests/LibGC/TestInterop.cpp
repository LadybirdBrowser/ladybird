/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestInterop.h"
#include "TestHeap.h"
#include <AK/TypeCasts.h>
#include <LibGC-Swift.h>
#include <LibGC/ForeignCell.h>
#include <LibGC/Heap.h>

#define COLLECT heap.collect_garbage(GC::Heap::CollectionType::CollectGarbage)
#define COLLECT_ALL heap.collect_garbage(GC::Heap::CollectionType::CollectEverything)

void test_interop()
{
    auto& heap = test_gc_heap();

    COLLECT_ALL;

    auto string = GC::ForeignRef<GC::HeapString>::allocate(heap, "Hello, World!");

    COLLECT;

    auto strings_string = std::string(string->getString());
    VERIFY(strings_string == "Hello, World!");

    COLLECT;

    auto* cell = string->getCell();
    VERIFY(cell == static_cast<GC::Cell*>(string.cell()));

    COLLECT;

    strings_string = std::string(string->getString());

    COLLECT;

    VERIFY(strings_string == "Hello, World!");

    COLLECT_ALL;
}
