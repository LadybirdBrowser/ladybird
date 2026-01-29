/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/StackInfo.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Root.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>
#include <LibGC/WeakBlock.h>
#include <LibGC/WeakContainer.h>

namespace GC {

struct StackFrameInfo {
    String label;
    size_t size_bytes { 0 };
};

class GC_API Heap {
    AK_MAKE_NONCOPYABLE(Heap);
    AK_MAKE_NONMOVABLE(Heap);

public:
    explicit Heap(AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots);
    ~Heap();

    static Heap& the();

    template<typename T, typename... Args>
    Ref<T> allocate(Args&&... args)
    {
        auto* memory = allocate_cell<T>();
        defer_gc();
        new (memory) T(forward<Args>(args)...);
        auto* cell = static_cast<T*>(memory);
        // Cells allocated during incremental sweep must be marked so they
        // survive until the next GC cycle clears and re-establishes marks.
        if (m_incremental_sweep_active) {
            cell->set_marked(true);
            m_cells_allocated_during_sweep.append(cell);
        }
        undefer_gc();
        return *cell;
    }

    enum class CollectionType {
        CollectGarbage,
        CollectEverything,
    };

    void collect_garbage(CollectionType = CollectionType::CollectGarbage, bool print_report = false);
    AK::JsonObject dump_graph();

    bool should_collect_on_every_allocation() const { return m_should_collect_on_every_allocation; }
    void set_should_collect_on_every_allocation(bool b) { m_should_collect_on_every_allocation = b; }

    void did_create_root(Badge<RootImpl>, RootImpl&);
    void did_destroy_root(Badge<RootImpl>, RootImpl&);

    void did_create_root_vector(Badge<RootVectorBase>, RootVectorBase&);
    void did_destroy_root_vector(Badge<RootVectorBase>, RootVectorBase&);

    void did_create_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase&);
    void did_destroy_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase&);

    void did_create_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase&);
    void did_destroy_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase&);

    void did_create_weak_container(Badge<WeakContainer>, WeakContainer&);
    void did_destroy_weak_container(Badge<WeakContainer>, WeakContainer&);

    void register_sweep_callback(AK::Function<void()>);

    void register_cell_allocator(Badge<CellAllocator>, CellAllocator&);

    void uproot_cell(Cell* cell);

    bool is_gc_deferred() const { return m_gc_deferrals > 0; }
    bool is_incremental_sweep_active() const { return m_incremental_sweep_active; }

    void sweep_block(HeapBlock&);

    bool is_live_heap_block(HeapBlock* block) const { return m_live_heap_blocks.contains(block); }

    void enqueue_post_gc_task(AK::Function<void()>);

    WeakImpl* create_weak_impl(void*);

    void did_allocate_external_memory(size_t);
    void did_free_external_memory(size_t);

private:
    friend class CellAllocator;
    friend class HeapBlock;
    friend class MarkingVisitor;
    friend class GraphConstructorVisitor;
    friend class DeferGC;

    void defer_gc();
    void undefer_gc();

    void dump_allocators();

    template<typename T>
    Cell* allocate_cell()
    {
        static_assert(requires { T::cell_allocator.allocator.get().allocate_cell(*this); }, "GC cell type must declare its own allocator using GC_DECLARE_ALLOCATOR(ClassName)");
        static_assert(IsSame<T, typename decltype(T::cell_allocator)::CellType>,
            "GC cell allocator type mismatch");

        will_allocate(sizeof(T));
        return T::cell_allocator.allocator.get().allocate_cell(*this);
    }

    void will_allocate(size_t);
    void update_gc_bytes_threshold(size_t live_cell_bytes, size_t live_external_bytes);

    void find_min_and_max_block_addresses(FlatPtr& min_address, FlatPtr& max_address);
    void gather_roots(HashMap<Cell*, HeapRoot>&, HashTable<HeapBlock*>& all_live_heap_blocks, Vector<StackFrameInfo>* out_stack_frames = nullptr);
    void gather_conservative_roots(HashMap<Cell*, HeapRoot>&, HashTable<HeapBlock*> const& all_live_heap_blocks, Vector<StackFrameInfo>* out_stack_frames = nullptr);
    void gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>&, FlatPtr, FlatPtr min_block_address, FlatPtr max_block_address, FlatPtr stack_reference, FlatPtr stack_top);
    void mark_live_cells(HashMap<Cell*, HeapRoot> const& live_cells, HashTable<HeapBlock*> const& all_live_heap_blocks);
    void finalize_unmarked_cells();
    void sweep_dead_cells(bool print_report, Core::ElapsedTimer const&);
    void sweep_weak_blocks();
    void run_post_gc_tasks();

    bool sweep_next_block();
    void start_incremental_sweep();
    void finish_incremental_sweep();
    void finish_pending_incremental_sweep();
    void start_incremental_sweep_timer();
    void stop_incremental_sweep_timer();
    void sweep_on_timer();

    template<typename Callback>
    void for_each_block(Callback callback)
    {
        for (auto& allocator : m_all_cell_allocators) {
            if (allocator.for_each_block(callback) == IterationDecision::Break)
                return;
        }
    }

    size_t m_gc_bytes_threshold { 0 };
    size_t m_allocated_bytes_since_last_gc { 0 };

    bool m_should_collect_on_every_allocation { false };

    CellAllocator::List m_all_cell_allocators;

    RootImpl::List m_roots;
    RootVectorBase::List m_root_vectors;
    RootHashMapBase::List m_root_hash_maps;
    ConservativeVectorBase::List m_conservative_vectors;
    WeakContainer::List m_weak_containers;

    Vector<Ptr<Cell>> m_uprooted_cells;

    size_t m_gc_deferrals { 0 };
    bool m_should_gc_when_deferral_ends { false };

    bool m_collecting_garbage { false };
    StackInfo m_stack_info;
    AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> m_gather_embedder_roots;

    Vector<AK::Function<void()>> m_post_gc_tasks;
    Vector<AK::Function<void()>> m_sweep_callbacks;

    HashTable<HeapBlock*> m_live_heap_blocks;

    WeakBlock::List m_usable_weak_blocks;
    WeakBlock::List m_full_weak_blocks;

    bool m_incremental_sweep_active { false };
    size_t m_sweep_live_cell_bytes { 0 };
    size_t m_sweep_live_external_bytes { 0 };
    Vector<GC::Ptr<Cell>> m_cells_allocated_during_sweep;
    CellAllocator::SweepList m_allocators_to_sweep;
    RefPtr<Core::Timer> m_incremental_sweep_timer;
};

inline void Heap::did_create_root(Badge<RootImpl>, RootImpl& impl)
{
    VERIFY(!m_roots.contains(impl));
    m_roots.append(impl);
}

inline void Heap::did_destroy_root(Badge<RootImpl>, RootImpl& impl)
{
    VERIFY(m_roots.contains(impl));
    m_roots.remove(impl);
}

inline void Heap::did_create_root_vector(Badge<RootVectorBase>, RootVectorBase& vector)
{
    VERIFY(!m_root_vectors.contains(vector));
    m_root_vectors.append(vector);
}

inline void Heap::did_destroy_root_vector(Badge<RootVectorBase>, RootVectorBase& vector)
{
    VERIFY(m_root_vectors.contains(vector));
    m_root_vectors.remove(vector);
}

inline void Heap::did_create_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase& hash_map)
{
    VERIFY(!m_root_hash_maps.contains(hash_map));
    m_root_hash_maps.append(hash_map);
}

inline void Heap::did_destroy_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase& hash_map)
{
    VERIFY(m_root_hash_maps.contains(hash_map));
    m_root_hash_maps.remove(hash_map);
}

inline void Heap::did_create_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase& vector)
{
    VERIFY(!m_conservative_vectors.contains(vector));
    m_conservative_vectors.append(vector);
}

inline void Heap::did_destroy_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase& vector)
{
    VERIFY(m_conservative_vectors.contains(vector));
    m_conservative_vectors.remove(vector);
}

inline void Heap::did_create_weak_container(Badge<WeakContainer>, WeakContainer& set)
{
    VERIFY(!m_weak_containers.contains(set));
    m_weak_containers.append(set);
}

inline void Heap::did_destroy_weak_container(Badge<WeakContainer>, WeakContainer& set)
{
    VERIFY(m_weak_containers.contains(set));
    m_weak_containers.remove(set);
}

inline void Heap::register_cell_allocator(Badge<CellAllocator>, CellAllocator& allocator)
{
    m_all_cell_allocators.append(allocator);
}

}
