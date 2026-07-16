/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWasm/AbstractMachine/ValueStack.h>

#if defined(AK_OS_WINDOWS)
#    include <windows.h>
#else
#    include <sys/mman.h>
#endif

namespace Wasm {

namespace {

void* allocate_region()
{
#if defined(AK_OS_WINDOWS)
    auto* region = VirtualAlloc(nullptr, ValueStack::reservation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VERIFY(region);
#else
    auto* region = mmap(nullptr, ValueStack::reservation_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VERIFY(region != MAP_FAILED);
#endif
    return region;
}

void free_region(void* region)
{
#if defined(AK_OS_WINDOWS)
    VirtualFree(region, 0, MEM_RELEASE);
#else
    munmap(region, ValueStack::reservation_size);
#endif
}

// The pool is intentionally leaked at thread exit to avoid unmapping during teardown.
struct RegionPool {
    static constexpr size_t max_pooled_regions = 4;
    Vector<void*, max_pooled_regions> regions;
};

RegionPool& region_pool()
{
    static thread_local auto* pool = new RegionPool;
    return *pool;
}

}

ValueStack::ValueStack()
{
    auto& pool = region_pool();
    void* region = pool.regions.is_empty() ? allocate_region() : pool.regions.take_last();
    m_base = static_cast<Value*>(region);
    m_top = m_base;
    m_limit = m_base + max_values;
}

ValueStack::~ValueStack()
{
    auto& pool = region_pool();
    if (pool.regions.size() < RegionPool::max_pooled_regions)
        pool.regions.unchecked_append(m_base);
    else
        free_region(m_base);
}

}
