/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/IntrusiveList.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StackInfo.h>
#include <AK/Swift.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Forward.h>
#include <LibGC/HeapRoot.h>
#include <LibGC/Internals.h>
#include <LibGC/MarkedVector.h>
#include <LibGC/Root.h>
#include <LibGC/WeakContainer.h>

namespace GC {

class Heap : public HeapBase {
    AK_MAKE_NONCOPYABLE(Heap);
    AK_MAKE_NONMOVABLE(Heap);

public:
    explicit Heap(void* private_data, AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> gather_embedder_roots);
    ~Heap();

    template<typename T, typename... Args>
    Ref<T> allocate(Args&&... args)
    {
        auto* memory = allocate_cell<T>();
        defer_gc();
        new (memory) T(forward<Args>(args)...);
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

    void did_create_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase&);
    void did_destroy_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase&);

    void did_create_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase&);
    void did_destroy_conservative_vector(Badge<ConservativeVectorBase>, ConservativeVectorBase&);

    void did_create_weak_container(Badge<WeakContainer>, WeakContainer&);
    void did_destroy_weak_container(Badge<WeakContainer>, WeakContainer&);

    void register_cell_allocator(Badge<CellAllocator>, CellAllocator&);

    void uproot_cell(Cell* cell);

    bool is_gc_deferred() const { return m_gc_deferrals > 0; }

private:
    friend class MarkingVisitor;
    friend class GraphConstructorVisitor;
    friend class DeferGC;
    friend class ForeignCell;

    void defer_gc();
    void undefer_gc();

    static bool cell_must_survive_garbage_collection(Cell const&);

    template<typename T>
    Cell* allocate_cell()
    {
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
    void gather_roots(HashMap<Cell*, HeapRoot>&);
    void gather_conservative_roots(HashMap<Cell*, HeapRoot>&);
    void gather_asan_fake_stack_roots(HashMap<FlatPtr, HeapRoot>&, FlatPtr, FlatPtr min_block_address, FlatPtr max_block_address);
    void mark_live_cells(HashMap<Cell*, HeapRoot> const& live_cells);
    void finalize_unmarked_cells();
    void sweep_dead_cells(bool print_report, Core::ElapsedTimer const&);

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

    bool m_should_collect_on_every_allocation { false };

    Vector<NonnullOwnPtr<CellAllocator>> m_size_based_cell_allocators;
    CellAllocator::List m_all_cell_allocators;

    RootImpl::List m_roots;
    MarkedVectorBase::List m_marked_vectors;
    ConservativeVectorBase::List m_conservative_vectors;
    WeakContainer::List m_weak_containers;

    Vector<Ptr<Cell>> m_uprooted_cells;

    size_t m_gc_deferrals { 0 };
    bool m_should_gc_when_deferral_ends { false };

    bool m_collecting_garbage { false };
    StackInfo m_stack_info;
    AK::Function<void(HashMap<Cell*, GC::HeapRoot>&)> m_gather_embedder_roots;
} SWIFT_IMMORTAL_REFERENCE;

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

inline void Heap::did_create_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase& vector)
{
    VERIFY(!m_marked_vectors.contains(vector));
    m_marked_vectors.append(vector);
}

inline void Heap::did_destroy_marked_vector(Badge<MarkedVectorBase>, MarkedVectorBase& vector)
{
    VERIFY(m_marked_vectors.contains(vector));
    m_marked_vectors.remove(vector);
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
