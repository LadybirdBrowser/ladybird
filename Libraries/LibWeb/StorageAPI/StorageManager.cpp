/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/StorageAPI/StorageManager.h>

namespace Web::StorageAPI {

GC_DEFINE_ALLOCATOR(StorageManager);

GC::Ref<StorageManager> StorageManager::create()
{
    return GC::Heap::the().allocate<StorageManager>();
}

StorageManager::StorageManager()
    : Bindings::Wrappable()
{
}

}
