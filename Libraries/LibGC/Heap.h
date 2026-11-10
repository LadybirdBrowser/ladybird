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
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/StackInfo.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/CrossHeapMember.h>
#include <LibGC/Forward.h>
#include <LibGC/FunctionTable.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/IdleCollectionPolicy.h>
#include <LibGC/Root.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibGC/WeakBlock.h>
#include <LibGC/WeakContainer.h>
#include <pthread.h>

namespace GC {

struct StackFrameInfo {
    String label;
    size_t size_bytes { 0 };
};

class GC_API Heap {
    AK_MAKE_NONCOPYABLE(Heap);
    AK_MAKE_NONMOVABLE(Heap);

    friend class HeapGroup;

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class BecomeThreadDefault {
        No,
        Yes,
    };

    explicit Heap(AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots, BecomeThreadDefault = BecomeThreadDefault::Yes);
    ~Heap();

    // The default heap of the calling thread.
    static Heap& the();
    static void set_default_heap_for_testing(Heap&);

    template<typename T, typename... Args>
    Ref<T> allocate(Args&&... args)
    {
        return construct_cell<T>(allocate_cell<T>(), forward<Args>(args)...);
    }

    // Like allocate(), but takes the cell from the given allocator instead of T's own. This lets one C++ type keep
    // several kinds of objects apart, each in blocks of its own.
    template<typename T, typename... Args>
    Ref<T> allocate_with_descriptor(TypeIsolatingCellAllocator<T>& descriptor, Args&&... args)
    {
        return construct_cell<T>(allocate_cell(descriptor), forward<Args>(args)...);
    }

    enum class CollectionType {
        CollectGarbage,
        CollectEverything,
    };

    // NB: NEVER_INLINE so that each entry point keeps a frame of its own: setjmp() only captures the registers as
    //     they are in the frame it runs in, and the frames below become the scan's floor. The definitions are
    //     NO_SANITIZE_ADDRESS so that the capture buffer stays on the real stack rather than ASan's fake one.
    NEVER_INLINE void collect_garbage(CollectionType = CollectionType::CollectGarbage, bool print_report = false);
    NEVER_INLINE AK::JsonObject dump_graph();

    // A cell on a retaining path, which reaches the next one through a field it visits.
    struct RetainingPathStep {
        StringView class_name;
    };
    struct RetainingPath {
        HeapRoot root;
        // The first step is the root cell, and the last is the target.
        Vector<RetainingPathStep> steps;
    };
    // The shortest path from a root to each reachable cell that is_target accepts, for at most max_paths of them.
    NEVER_INLINE Vector<RetainingPath> find_retaining_paths(AK::Function<bool(Cell&)> const& is_target, size_t max_paths);
    static String describe_root(HeapRoot const&);

    // A heap belongs to the thread that created it: the collector only scans that thread's stack, and nothing in the heap
    // is synchronized. That thread has to destroy it as well. Collections and destruction verify this, allocation and
    // rooting are debug assertions.
    bool is_on_owner_thread() const { return pthread_equal(m_owner_thread, pthread_self()); }

    bool should_collect_on_every_allocation() const { return m_should_collect_on_every_allocation; }
    // This is true for any CollectEverything cycle, not only heap teardown.
    bool is_collecting_everything() const { return m_collecting_garbage && m_current_collection_type == CollectionType::CollectEverything; }

    void set_incremental_sweep_enabled(bool enabled) { m_incremental_sweep_enabled = enabled; }
    void set_should_collect_on_every_allocation(bool);

    void did_create_root(Badge<RootImpl>, RootImpl&);
    void did_destroy_root(Badge<RootImpl>, RootImpl&);

    void did_create_root_vector(Badge<RootVectorBase>, RootVectorBase&);
    void did_destroy_root_vector(Badge<RootVectorBase>, RootVectorBase&);

    void did_create_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase&);
    void did_destroy_root_hash_map(Badge<RootHashMapBase>, RootHashMapBase&);
    void did_create_root_hash_table(Badge<RootHashTableBase>, RootHashTableBase&);
    void did_destroy_root_hash_table(Badge<RootHashTableBase>, RootHashTableBase&);

    void did_create_cross_heap_member(Badge<CrossHeapMemberBase>, CrossHeapMemberBase&);
    void did_destroy_cross_heap_member(Badge<CrossHeapMemberBase>, CrossHeapMemberBase&);

    void did_create_weak_container(Badge<WeakContainer>, WeakContainer&);
    void did_destroy_weak_container(Badge<WeakContainer>, WeakContainer&);

    void enable_stats();
    void add_stats(AK::JsonObject&) const;

    // Per allocator: its blocks, its live cells and bytes, and per class the live cells and the edges to cells they
    // visit. Finishes any pending sweep first.
    AK::JsonObject census();

    void register_sweep_callback(AK::Function<void()>);

    void register_cell_allocator(Badge<CellAllocator>, CellAllocator&);

    void uproot_cell(Cell* cell);
    void uproot_cell(ForeignCell* cell) { uproot_cell(as_cell(cell)); }

    bool is_gc_deferred() const { return m_gc_deferrals > 0; }

    FunctionTable& function_table() { return m_function_table; }
    bool is_incremental_sweep_active() const { return m_incremental_sweep_active; }

    void sweep_block(HeapBlock&);

    bool is_live_heap_block(HeapBlock* block) const { return m_live_heap_blocks.contains(block); }

    template<typename Callback>
    void for_each_block(Callback callback)
    {
        for (auto& allocator : m_all_cell_allocators) {
            if (allocator.for_each_block(callback) == IterationDecision::Break)
                return;
        }
    }

    void enqueue_post_gc_task(AK::Function<void()>);

    WeakImpl* create_weak_impl(void*);

    void did_allocate_external_memory(size_t);
    void did_free_external_memory(size_t);

    // Tells the heap that a large graph of objects, such as a page that went away, has likely just become garbage.
    // Nothing about allocation shows that, so this brings the next collection forward, and hands what that collection
    // frees back to the system once it has been swept. It never collects from here, since the graph is often still on
    // the caller's stack.
    void did_abandon_object_graph();

private:
    friend struct CAPI;
    friend class CellAllocator;
    friend class HeapBlock;
    friend class MarkingVisitor;
    friend class DeferGC;

    void defer_gc();
    void undefer_gc();

    void dump_allocators();
    void finish_stats_collection();

    template<typename T, typename... Args>
    Ref<T> construct_cell(Cell* memory, Args&&... args)
    {
        defer_gc();
        new (memory) T(forward<Args>(args)...);
        auto* cell = static_cast<T*>(memory);
        cell->set_cell_kind(T::cell_kind_for_class);
        mark_if_allocated_during_incremental_sweep(*cell);
        undefer_gc();
        return *cell;
    }

    CellAllocator& cell_allocator_for(CellAllocatorDescriptorBase& descriptor)
    {
        auto index = descriptor.index();
        if (index < m_cell_allocators_by_descriptor_index.size()) [[likely]] {
            if (auto* allocator = m_cell_allocators_by_descriptor_index[index].ptr()) [[likely]]
                return *allocator;
        }
        return create_cell_allocator(descriptor);
    }
    CellAllocator& create_cell_allocator(CellAllocatorDescriptorBase&);

    template<typename T>
    Cell* allocate_cell()
    {
        static_assert(requires { cell_allocator_for(T::cell_allocator).allocate_cell(*this); }, "GC cell type must declare its own allocator using GC_DECLARE_ALLOCATOR(ClassName)");
        static_assert(IsSame<T, typename decltype(T::cell_allocator)::CellType>,
            "GC cell allocator type mismatch");

        return allocate_cell(T::cell_allocator);
    }

    // Shared by allocate<T>() and any allocation whose cell type is only known through its allocator descriptor.
    // Cells that other cells keep their storage in never collect garbage, but count towards the next collection (see
    // gc_heap_allocate_storage_cell()).
    enum class TriggersCollection {
        No,
        Yes,
    };
    Cell* allocate_cell(CellAllocatorDescriptorBase& descriptor, TriggersCollection triggers_collection = TriggersCollection::Yes)
    {
        VERIFY(!m_collecting_garbage);
        will_allocate(descriptor.cell_size(), triggers_collection);
        return cell_allocator_for(descriptor).allocate_cell(*this);
    }

    // Cells allocated during incremental sweep in a block that the sweep has
    // yet to finish must be marked so that it keeps them. The sweep clears the
    // marks of the blocks it has yet to sweep; cells in the block it is
    // sweeping right now are remembered so their marks get cleared when the
    // sweep finishes. Swept and new blocks need nothing.
    bool mark_if_allocated_during_incremental_sweep(Cell& cell)
    {
        if (!m_incremental_sweep_active)
            return false;
        auto* block = HeapBlock::from_cell(&cell);
        if (block->is_pending_sweep()) {
            cell.set_marked(true);
            return true;
        }
        if (block->is_being_swept()) {
            cell.set_marked(true);
            m_cells_allocated_during_sweep.append(&cell);
            return true;
        }
        return false;
    }

    // Marking makes every block pending sweep again, which local free lists
    // must never be in, so every collection does this once it has finished the
    // pending sweep, whose destructors may take new local free lists.
    void give_back_local_free_lists();

    void will_allocate(size_t, TriggersCollection = TriggersCollection::Yes);
    void update_allocation_rate();
    double next_growth_factor(size_t live_cell_bytes);
    void update_gc_bytes_threshold(size_t live_external_bytes);
    void limit_gc_bytes_threshold_for_abandoned_object_graph();

    enum class IncludeIncomingCrossHeapMembers {
        No,
        Yes,
    };
    // Where the conservative scan starts: every frame from stack_floor up, plus the callee-saved registers as captured
    // at the entry point. The floor lands just below the entry point's frame, so the caller's registers that its
    // prologue spilled are covered by the stack scan and the ones it left alone by the capture.
    struct ConservativeScanOrigin {
        FlatPtr stack_floor { 0 };
        ReadonlySpan<FlatPtr> callee_saved_registers;
    };

    // NB: NEVER_INLINE so that these stay below their entry point's frame, and so that __builtin_frame_address(0)
    //     yields a floor that leaves out their own frames -- which is what keeps the collection's large locals from
    //     being scanned.
    NEVER_INLINE void run_collection(ReadonlySpan<FlatPtr> callee_saved_registers, CollectionType, bool print_report);
    NEVER_INLINE AK::JsonObject build_graph(ReadonlySpan<FlatPtr> callee_saved_registers);
    NEVER_INLINE Vector<RetainingPath> build_retaining_paths(ReadonlySpan<FlatPtr> callee_saved_registers, AK::Function<bool(Cell&)> const& is_target, size_t max_paths);

    void gather_roots(ConservativeScanOrigin const&, HashMap<Cell*, HeapRoot>&, Vector<StackFrameInfo>* out_stack_frames = nullptr, IncludeIncomingCrossHeapMembers = IncludeIncomingCrossHeapMembers::Yes);
    static void mark_live_cells_across(ReadonlySpan<Heap* const>, HashMap<Cell*, HeapRoot>& roots);
    void clear_weak_pointers_to_dead_cells(bool report);
    void run_post_mark_phases(bool report);
    void gather_conservative_roots(ConservativeScanOrigin const&, HashMap<Cell*, HeapRoot>&, Vector<StackFrameInfo>* out_stack_frames = nullptr);
    void mark_live_cells(HashMap<Cell*, HeapRoot>& roots);
    void finalize_unmarked_cells();
    void sweep_dead_cells(bool print_report, Core::ElapsedTimer const&);
    void sweep_weak_blocks();
    void free_weak_blocks();
    void run_post_gc_tasks();

    bool sweep_next_block();
    void start_incremental_sweep();
    void finish_incremental_sweep();
    void finish_pending_incremental_sweep();
    void start_incremental_sweep_timer();
    void stop_incremental_sweep_timer();
    void sweep_on_timer();

    void start_idle_gc_timer();
    void idle_gc_on_timer();
    void on_abandoned_object_graph_timer();
    void schedule_release_of_abandoned_object_graph_memory();

    struct Stats {
        AK_ALLOC_WITH_KMALLOC;

        u64 collections { 0 };
        u64 completed_collections { 0 };
        u64 total_duration_ns { 0 };
        u64 last_duration_ns { 0 };
        u64 pause_duration_ns { 0 };
        u64 sweep_duration_ns { 0 };
        size_t bytes_before { 0 };
        size_t live_cell_bytes { 0 };
        size_t live_external_bytes { 0 };
    };
    OwnPtr<Stats> m_stats;

    pthread_t m_owner_thread { pthread_self() };

    size_t m_gc_bytes_threshold { 0 };
    size_t m_allocated_bytes_since_last_gc { 0 };

    bool m_should_collect_on_every_allocation { false };

    CellAllocator::List m_all_cell_allocators;
    Vector<OwnPtr<CellAllocator>> m_cell_allocators_by_descriptor_index;

    RootImpl::List m_roots;
    RootVectorBase::List m_root_vectors;
    RootHashMapBase::List m_root_hash_maps;
    RootHashTableBase::List m_root_hash_tables;
    HashTable<CrossHeapMemberBase*> m_incoming_cross_heap_members;
    HeapGroup* m_group { nullptr };
    bool m_incremental_sweep_enabled { true };
    WeakContainer::List m_weak_containers;

    Vector<Ptr<Cell>> m_uprooted_cells;

    size_t m_gc_deferrals { 0 };
    bool m_should_gc_when_deferral_ends { false };

    bool m_collecting_garbage { false };
    bool m_weak_pointers_to_dead_cells_are_cleared { false };
    CollectionType m_current_collection_type { CollectionType::CollectGarbage };
    StackInfo m_stack_info;
    FunctionTable m_function_table;
    AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> m_gather_embedder_roots;

    Vector<AK::Function<void()>> m_post_gc_tasks;
    Vector<AK::Function<void()>> m_sweep_callbacks;

    HashTable<HeapBlock*> m_live_heap_blocks;

    WeakBlock::List m_usable_weak_blocks;
    WeakBlock::List m_full_weak_blocks;

    bool m_incremental_sweep_active { false };
    size_t m_block_sweeps_in_progress { 0 };
    size_t m_marked_cell_bytes { 0 };
    size_t m_sweep_live_external_bytes { 0 };
    Vector<GC::Ptr<Cell>> m_cells_allocated_during_sweep;
    CellAllocator::SweepList m_allocators_to_sweep;
    RefPtr<Core::Timer> m_incremental_sweep_timer;

    RefPtr<Core::Timer> m_idle_gc_timer;
    enum class AbandonedObjectGraphState : u8 {
        None,
        WaitingToCollect,
        WaitingToReleaseMemory,
    };
    AbandonedObjectGraphState m_abandoned_object_graph_state { AbandonedObjectGraphState::None };
    RefPtr<Core::Timer> m_abandoned_object_graph_timer;
    u64 m_total_allocated_bytes { 0 };
    // The context a heap created through LibGC/CAPI.h was created with.
    void* m_foreign_context { nullptr };
    IdleCollectionPolicy m_idle_collection_policy;

    // What decides the next collection budget. See next_growth_factor().
    double m_allocation_bytes_per_second { 0 };
    Optional<double> m_wanted_growth_factor;
    u64 m_total_allocated_bytes_at_last_collection { 0 };
    i64 m_last_collection_end_ns { 0 };
    i64 m_mark_duration_ns { 0 };
    i64 m_sweep_duration_ns { 0 };
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

inline void Heap::did_create_root_hash_table(Badge<RootHashTableBase>, RootHashTableBase& hash_table)
{
    VERIFY(!m_root_hash_tables.contains(hash_table));
    m_root_hash_tables.append(hash_table);
}

inline void Heap::did_destroy_root_hash_table(Badge<RootHashTableBase>, RootHashTableBase& hash_table)
{
    VERIFY(m_root_hash_tables.contains(hash_table));
    m_root_hash_tables.remove(hash_table);
}

inline void Heap::did_create_cross_heap_member(Badge<CrossHeapMemberBase>, CrossHeapMemberBase& member)
{
    VERIFY(!m_incoming_cross_heap_members.contains(&member));
    m_incoming_cross_heap_members.set(&member);
}

inline void Heap::did_destroy_cross_heap_member(Badge<CrossHeapMemberBase>, CrossHeapMemberBase& member)
{
    VERIFY(m_incoming_cross_heap_members.contains(&member));
    m_incoming_cross_heap_members.remove(&member);
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
