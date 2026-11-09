/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGC/Forward.h>

namespace GC {

enum class DeferDecommit {
    No,
    Yes,
};

class DecommitWorker;

// The blocks of one type of cell in all heaps, see CellAllocatorDescriptorBase::block_allocator().
class GC_API BlockAllocator {
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

private:
    friend class DecommitWorker;

    // Slots in "ready to reuse" state -- have been MADV_FREE_REUSABLE'd by
    // the worker (Darwin) so allocate_block pairs them with MADV_FREE_REUSE.
    Vector<void*> m_blocks;

    // Slots whose pages went back for real, as their heap went away or they
    // stayed unused for long. They are the last ones to reuse, as their pages
    // have to be faulted in again.
    Vector<void*> m_returned_blocks;

    // Slots freed by deallocate_block but not yet madvised. allocate_block
    // pops directly from here to skip the madvise round-trip on hot recycle
    // paths -- this is the main payoff of deferring decommit.
    Vector<void*> m_freshly_freed;

    // Protects the slots above and m_in_decommit_registry. Held
    // briefly on the alloc/dealloc hot path; uncontended in the common case.
    Mutex m_mutex;

    // Refcount the decommit worker bumps while it has a reference to this
    // allocator. The destructor waits on m_worker_cv until it hits zero so
    // we never let our storage go away while the worker is still running.
    AK::Atomic<int> m_worker_refcount { 0 };
    ConditionVariable m_worker_cv;

    // True iff this allocator is currently in the worker's pending list.
    // Avoids re-registering on every dealloc; cleared by the worker at the
    // start of process_one, set by deallocate_block under m_mutex, and by
    // process_one for blocks that it has to look at again.
    bool m_in_decommit_registry { false };
};

}
