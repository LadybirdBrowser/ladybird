/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakContainer.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

// A set of cells that a cell keeps weakly, such as a document's live ranges, which libweb_rust reads in place through a
// RustFFI::WeakCellRegistryField of the owner's core. See Rust/src/dom/weak_cells.rs.
//
// The cells are held by GC::Weak pointers, so the registry keeps none of them alive, and the collector points each at
// the new address of a cell it moves, and nulls one whose cell dies. After every collection, the registry drops the
// null entries. Whenever it changes its entries, it mirrors where they are and how many into its span.
//
// Each cell keeps the index of its entry, which T exposes as weak_cell_registry_index(), so that removing a cell nulls
// its entry in constant time, and keeps the order of the others. The registry drops the null entries once they are as
// many as the others, and after every collection.
//
// The owner keeps the registry as a LazyWeakCellRegistry, which allocates it as the first cell is added, and points the
// field of its core at the span for as long as it lives.
template<typename T>
class WeakCellRegistry final : public GC::WeakContainer {
    AK_MAKE_NONCOPYABLE(WeakCellRegistry);
    AK_MAKE_NONMOVABLE(WeakCellRegistry);
    AK_ALLOC_WITH_KMALLOC;

public:
    explicit WeakCellRegistry(GC::Cell const& owner)
        : GC::WeakContainer(owner.heap())
        , m_owner(owner)
    {
    }

    RustFFI::WeakCellSpan const& span() const { return m_span; }

    void add(T& cell)
    {
        cell.weak_cell_registry_index() = m_cells.size();
        m_cells.append(cell);
        mirror();
    }

    void remove(T& cell)
    {
        auto index = cell.weak_cell_registry_index();
        VERIFY(index < m_cells.size() && m_cells[index].ptr().ptr() == &cell);
        m_cells[index] = {};
        ++m_removed_count;
        if (m_removed_count >= m_cells.size() - m_removed_count)
            remove_null_entries();
    }

    bool is_empty() const { return m_cells.is_empty(); }

    // NB: The registry names its owner, which is what visits it, so this marks nothing the owner's visit does not.
    void visit_edges(GC::Cell::Visitor& visitor) const { visitor.visit(*m_owner); }

    template<typename Callback>
    void for_each(Callback callback) const
    {
        for (auto const& entry : m_cells) {
            if (auto cell = entry.ptr())
                callback(*cell);
        }
    }

    virtual void remove_dead_cells(Badge<GC::Heap>) override
    {
        remove_null_entries();
    }

    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override { return *m_owner; }

private:
    void remove_null_entries()
    {
        m_cells.remove_all_matching([](auto const& entry) { return !entry; });
        for (size_t index = 0; index < m_cells.size(); ++index)
            m_cells[index]->weak_cell_registry_index() = index;
        m_removed_count = 0;
        mirror();
    }

    void mirror()
    {
        m_span.entries = reinterpret_cast<void const* const*>(m_cells.data());
        m_span.length = m_cells.size();
    }

    GC::Ref<GC::Cell const> m_owner;
    RustFFI::WeakCellSpan m_span {};
    Vector<GC::Weak<T>> m_cells;
    // How many entries remove() nulled since the null entries were last dropped.
    size_t m_removed_count { 0 };
};

// A WeakCellRegistry that its owner allocates out of line once it adds the first cell, so that an owner that never adds
// one spends a pointer on it, and the field of the owner's core that libweb_rust reads it through another.
template<typename T>
class LazyWeakCellRegistry {
    AK_MAKE_NONCOPYABLE(LazyWeakCellRegistry);
    AK_MAKE_NONMOVABLE(LazyWeakCellRegistry);

public:
    LazyWeakCellRegistry() = default;

    // `field` is the field of the owner's core that points at the registry's span, which this sets as it allocates the
    // registry. The owner passes the same field every time.
    void add(GC::Cell const& owner, RustFFI::WeakCellSpan const*& field, T& cell)
    {
        if (!m_registry) {
            m_registry = make<WeakCellRegistry<T>>(owner);
            field = &m_registry->span();
        }
        m_registry->add(cell);
    }

    void remove(T& cell)
    {
        VERIFY(m_registry);
        m_registry->remove(cell);
    }

    bool is_empty() const { return !m_registry || m_registry->is_empty(); }

    void visit_edges(GC::Cell::Visitor& visitor) const
    {
        if (m_registry)
            m_registry->visit_edges(visitor);
    }

    template<typename Callback>
    void for_each(Callback callback) const
    {
        if (m_registry)
            m_registry->for_each(move(callback));
    }

private:
    OwnPtr<WeakCellRegistry<T>> m_registry;
};

// NB: The Rust definition of the span asserts the same layout, and reads each entry as a pointer to its GC::WeakImpl.
static_assert(sizeof(RustFFI::WeakCellSpan) == 16 && alignof(RustFFI::WeakCellSpan) == 8);
static_assert(offsetof(RustFFI::WeakCellSpan, entries) == 0);
static_assert(offsetof(RustFFI::WeakCellSpan, length) == 8);
static_assert(sizeof(RustFFI::WeakCellRegistryField) == sizeof(void*));
static_assert(sizeof(GC::Weak<GC::Cell>) == sizeof(void*));
static_assert(GC::WeakImpl::value_offset() == RustFFI::WEAK_IMPL_VALUE_OFFSET);

}
