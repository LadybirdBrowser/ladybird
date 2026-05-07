/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace GC {

class DecommitWorker;

class GC_API BlockAllocator {
public:
    BlockAllocator();
    ~BlockAllocator();

    void* allocate_block(char const* name);
    void deallocate_block(void*);

    size_t block_count();

    // Wake the global decommit worker so it processes any deferred madvise
    // work that's piled up. Call this at the end of a GC sweep.
    static void wake_decommit_worker_async();

private:
    friend class DecommitWorker;

    // Slots in "ready to reuse" state -- have been MADV_FREE_REUSABLE'd by
    // the worker (Darwin) so allocate_block pairs them with MADV_FREE_REUSE.
    Vector<void*> m_blocks;

    // Slots freed by deallocate_block but not yet madvised. allocate_block
    // pops directly from here to skip the madvise round-trip on hot recycle
    // paths -- this is the main payoff of deferring decommit.
    Vector<void*> m_freshly_freed;

    // Protects m_blocks, m_freshly_freed, and m_in_decommit_registry. Held
    // briefly on the alloc/dealloc hot path; uncontended in the common case.
    Threading::Mutex m_mutex;

    // Refcount the decommit worker bumps while it has a reference to this
    // allocator. The destructor waits on m_worker_cv until it hits zero so
    // we never let our storage go away while the worker is still running.
    AK::Atomic<int> m_worker_refcount { 0 };
    Threading::ConditionVariable m_worker_cv;

    // True iff this allocator is currently in the worker's pending list.
    // Avoids re-registering on every dealloc; cleared by the worker at the
    // start of process_one, set by deallocate_block under m_mutex.
    bool m_in_decommit_registry { false };
};

}
