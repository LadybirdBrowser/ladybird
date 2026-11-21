/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Noncopyable.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibGC/Forward.h>

namespace GC::FFI {

struct BlockPool;

}

namespace GC {

enum class DeferDecommit {
    No,
    Yes,
};

// The blocks of one type of cell in all heaps, see CellAllocatorDescriptorBase::block_allocator(). The pool itself, the
// heap region's chunks and the thread that gives the pages of freed blocks back to the system are LibGC's Rust core.
class GC_API BlockAllocator {
    AK_MAKE_NONCOPYABLE(BlockAllocator);
    AK_MAKE_NONMOVABLE(BlockAllocator);

public:
    AK_ALLOC_WITH_KMALLOC;

    BlockAllocator();
    ~BlockAllocator();

    void* allocate_block(char const* name);
    void deallocate_block(void*, DeferDecommit = DeferDecommit::Yes);

    size_t block_count();

    // How many blocks are back in the pool after their pages went back to the system. Tests wait on it.
    size_t returned_block_count_for_testing();

    // Gives the pages of the free blocks back to the system right away, rather than leaving them for the system to take
    // when it runs short of memory. For a heap that went away, whose blocks may never be reused.
    void return_freed_blocks_to_system();

    static FlatPtr heap_region_start();
    static FlatPtr heap_region_end();
    // The end of the part of the region that chunks have been committed from so far. No block starts at or above it.
    static FlatPtr heap_region_committed_end();

    // Wake the global decommit worker so it processes any deferred madvise
    // work that's piled up. Call this at the end of a GC sweep.
    static void wake_decommit_worker_async();

    // How long a block can sit unused before its pages go back to the system. Tests make it short to see this happen.
    // Returns the age it replaces.
    static AK::Duration set_cold_block_age_for_testing(AK::Duration);

    FFI::BlockPool const* rust_pool(Badge<CellAllocator>) const { return m_pool; }

private:
    FFI::BlockPool const* m_pool { nullptr };
};

}
