/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
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

class MarkingVisitor;

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
        // Allocate-gray: cells created during incremental marking are
        // added to the marking worklist so their edges get traced.
        if (m_incremental_marking_in_progress) {
            memory->set_gc_color(GC::Color::Gray);
            enqueue_barrier_gray(*memory);
        } else if (m_incremental_sweep_in_progress) {
            // Allocate-black: cells created during incremental sweep
            // must not be swept.  finalize_incremental_sweep resets
            // their color to white before the next cycle starts.
            memory->set_gc_color(GC::Color::Black);
        }
        undefer_gc();
        return *static_cast<T*>(memory);
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
    void enqueue_barrier_gray(Cell& cell);

    bool is_gc_deferred() const { return m_gc_deferrals > 0; }

    // Advance any in-progress incremental mark or sweep by one budget step.
    // Intended to be called from an event-loop idle hook so cycles continue
    // to make progress even when the mutator stops allocating.
    void incremental_idle_step();

    void enqueue_post_gc_task(AK::Function<void()>);

    WeakImpl* create_weak_impl(void*);

private:
    friend class MarkingVisitor;
    friend class GraphConstructorVisitor;
    friend class DeferGC;

    void defer_gc();
    void undefer_gc();

    void dump_allocators();

    template<typename T>
    static consteval bool has_own_gc_allocator_marker()
    {
        if constexpr (requires { typename T::gc_allocator_marker; })
            return IsSame<typename T::gc_allocator_marker, T>;
        return false;
    }

    template<typename T>
    Cell* allocate_cell()
    {
        static_assert(has_own_gc_allocator_marker<T>(), "Cell type must declare its own allocator with either GC_DECLARE_ALLOCATOR (for type-isolated allocation) or GC_DECLARE_SIZE_BASED_ALLOCATOR (for size-based allocation)");

        will_allocate(sizeof(T));
        if constexpr (requires { T::cell_allocator.allocator.get().allocate_cell(*this); }) {
            if constexpr (IsSame<T, typename decltype(T::cell_allocator)::CellType>) {
                return T::cell_allocator.allocator.get().allocate_cell(*this);
            }
        }
        return allocator_for_size(sizeof(T)).allocate_cell(*this);
    }

    void will_allocate(size_t);

    void find_min_and_max_block_addresses(FlatPtr& min_address, FlatPtr& max_address);
    void gather_roots(HashMap<Cell*, HeapRoot>&, HashTable<HeapBlock*>& all_live_heap_blocks, Vector<StackFrameInfo>* out_stack_frames = nullptr);
    void gather_conservative_roots(HashMap<Cell*, HeapRoot>&, HashTable<HeapBlock*> const& all_live_heap_blocks, Vector<StackFrameInfo>* out_stack_frames = nullptr);
    void gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>&, FlatPtr, FlatPtr min_block_address, FlatPtr max_block_address);
    void mark_live_cells(HashMap<Cell*, HeapRoot> const& live_cells, HashTable<HeapBlock*> const& all_live_heap_blocks);
    void start_incremental_marking();
    bool continue_incremental_marking(size_t cell_budget);
    void finish_incremental_marking();
    void finalize_unmarked_cells();
    void sweep_dead_cells(bool print_report, Core::ElapsedTimer const&);
    void sweep_weak_blocks();
    void begin_incremental_sweep();
    bool continue_incremental_sweep(size_t block_budget);
    void finalize_incremental_sweep();
    void force_sweep_to_completion();
    void sweep_one_block(HeapBlock& block);
    void run_post_gc_tasks();

    ALWAYS_INLINE CellAllocator& allocator_for_size(size_t cell_size)
    {
        // FIXME: Use binary search?
        for (auto& allocator : m_size_based_cell_allocators) {
            if (allocator->cell_size() >= cell_size)
                return *allocator;
        }
        dbgln("Cannot get CellAllocator for cell size {}, largest available is {}!", cell_size, m_size_based_cell_allocators.last()->cell_size());
        VERIFY_NOT_REACHED();
    }

    template<typename Callback>
    void for_each_block(Callback callback)
    {
        for (auto& allocator : m_all_cell_allocators) {
            if (allocator.for_each_block(callback) == IterationDecision::Break)
                return;
        }
    }

    static constexpr size_t GC_MIN_BYTES_THRESHOLD { 4 * 1024 * 1024 };
    size_t m_gc_bytes_threshold { GC_MIN_BYTES_THRESHOLD };
    size_t m_allocated_bytes_since_last_gc { 0 };

    size_t m_incremental_marking_budget { 1000 };
    size_t m_incremental_sweep_budget { 4 };

    bool m_should_collect_on_every_allocation { false };

    Vector<NonnullOwnPtr<CellAllocator>> m_size_based_cell_allocators;
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

    // Incremental marking state.
    bool m_incremental_marking_in_progress { false };
    OwnPtr<MarkingVisitor> m_marking_visitor;
    HashTable<HeapBlock*> m_marking_heap_blocks;

    // Incremental sweep state.
    bool m_incremental_sweep_in_progress { false };
    Vector<HeapBlock*> m_sweep_blocks_remaining;
    size_t m_sweep_collected_cells { 0 };
    size_t m_sweep_live_cells { 0 };
    size_t m_sweep_collected_cell_bytes { 0 };
    size_t m_sweep_live_cell_bytes { 0 };
    size_t m_sweep_empty_block_count { 0 };
    StackInfo m_stack_info;
    AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> m_gather_embedder_roots;

    Vector<AK::Function<void()>> m_post_gc_tasks;
    Vector<AK::Function<void()>> m_sweep_callbacks;

    WeakBlock::List m_usable_weak_blocks;
    WeakBlock::List m_full_weak_blocks;
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
