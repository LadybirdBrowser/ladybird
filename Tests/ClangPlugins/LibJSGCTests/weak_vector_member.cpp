/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/HashMap.h>
#include <LibGC/Cell.h>
#include <LibGC/WeakContainer.h>
#include <LibGC/WeakVector.h>

struct Entry {
    GC::WeakVector<GC::Cell> objects;
};

struct DerivedEntry : Entry {
};

class PrunedCache final
    : public GC::Cell
    , public GC::WeakContainer {
    GC_CELL(PrunedCache, GC::Cell);

    PrunedCache()
        : GC::WeakContainer(heap())
    {
    }

    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override { return *this; }

    virtual void remove_dead_cells(Badge<GC::Heap>) override
    {
        m_objects.remove_dead_cells(heap());
        m_entries.remove_all_matching([&](int const&, Entry& entry) { return entry.objects.remove_dead_cells(heap()); });
    }

    GC::WeakVector<GC::Cell> m_objects;
    HashMap<int, Entry> m_entries;
};

class UnprunedCache final : public GC::Cell {
    GC_CELL(UnprunedCache, GC::Cell);

    // expected-error@+1 {{Member 'm_objects' holds a GC::WeakVector, but 'UnprunedCache' is not a GC::WeakContainer that removes its dead cells}}
    GC::WeakVector<GC::Cell> m_objects;

    // expected-error@+1 {{Member 'm_entries' holds a GC::WeakVector, but 'UnprunedCache' is not a GC::WeakContainer that removes its dead cells}}
    HashMap<int, Entry> m_entries;

    // expected-error@+1 {{Member 'm_object_array' holds a GC::WeakVector, but 'UnprunedCache' is not a GC::WeakContainer that removes its dead cells}}
    GC::WeakVector<GC::Cell> m_object_array[1];

    // expected-error@+1 {{Member 'm_derived_entry' holds a GC::WeakVector, but 'UnprunedCache' is not a GC::WeakContainer that removes its dead cells}}
    DerivedEntry m_derived_entry;
};

class UnprunedEntryCell final
    : public GC::Cell
    // expected-error@+1 {{Base 'Entry' holds a GC::WeakVector, but 'UnprunedEntryCell' is not a GC::WeakContainer that removes its dead cells}}
    , public Entry {
    GC_CELL(UnprunedEntryCell, GC::Cell);
};

template<typename T>
struct NotAWeakContainer {
};

template<typename T>
class TemplatedUnprunedCache final
    : public GC::Cell
    , public NotAWeakContainer<T> {
    GC_CELL(TemplatedUnprunedCache, GC::Cell);

    // expected-error@+1 {{Member 'm_objects' holds a GC::WeakVector, but 'TemplatedUnprunedCache' is not a GC::WeakContainer that removes its dead cells}}
    GC::WeakVector<GC::Cell> m_objects;
};

template<typename T>
struct WeakContainerBase : public GC::WeakContainer {
};

template<typename T>
class TemplatedPrunedCache final
    : public GC::Cell
    , public WeakContainerBase<T> {
    GC_CELL(TemplatedPrunedCache, GC::Cell);

    GC::WeakVector<GC::Cell> m_objects;
};
