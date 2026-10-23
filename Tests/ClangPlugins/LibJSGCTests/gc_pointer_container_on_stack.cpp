/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify -ferror-limit=0 %plugin_opts% -c %s -o %t 2>&1

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/NanBoxedValue.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/RootHashMap.h>
#include <LibGC/RootHashTable.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>

struct Entry {
    GC::Ref<GC::Cell> cell;
};

struct EntryWithoutGCPointers {
    int value;
};

struct State {
    Vector<GC::Ref<GC::Cell>> cells;
};

struct DerivedState : State { };

struct DerivedEntry : Entry { };

struct EntryWithArray {
    GC::Ptr<GC::Cell> cells[2];
};

Vector<GC::Ref<GC::Cell>> make_cells();

void unrooted_locals()
{
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::Ref<GC::Cell>> refs;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::Ptr<GC::Cell>, 4> ptrs_with_inline_capacity;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::Cell*> raw_pointers;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::NanBoxedValue> values;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<Optional<GC::Ref<GC::Cell>>> optional_refs;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<Vector<GC::Ref<GC::Cell>>> nested;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<Entry> entries;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    HashMap<int, GC::Ref<GC::Cell>> map_values;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    HashMap<GC::Ptr<GC::Cell>, int> map_keys;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    OrderedHashMap<int, GC::Ref<GC::Cell>> ordered_map;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    HashTable<GC::Ptr<GC::Cell>> table;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Optional<Vector<GC::Ref<GC::Cell>>> optional_vector;
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    State state;
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    DerivedState derived_state;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<DerivedEntry> derived_entries;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<EntryWithArray> entries_with_arrays;
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::Ref<GC::Cell>> groups[2];
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    GC::RootHashMap<GC::Ref<GC::Cell>, Vector<GC::Ref<GC::Cell>>> root_map_of_vectors;
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    GC::ConservativeVector<State> conservative_states;
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    auto deduced = make_cells();
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    auto const& lifetime_extended = make_cells();
}

template<typename T>
void dependent_local()
{
    // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
    Vector<GC::Ref<T>> refs;
}

void lambda_local()
{
    [] {
        // expected-error@+1 {{keeps GC pointers in heap memory that is not scanned for roots}}
        Vector<GC::Ref<GC::Cell>> refs;
    }();
}

void parameter_by_value(Vector<GC::Ref<GC::Cell>> cells, State& state)
{
    state.cells = move(cells);
}

void rooted_or_not_holding_gc_pointers(State& state)
{
    GC::RootVector<GC::Ref<GC::Cell>> root_vector;
    GC::RootVector<GC::Ref<GC::Cell>> root_groups[2];
    Vector<GC::Root<GC::Cell>> roots;
    GC::ConservativeVector<Entry> conservative_entries;
    GC::RootHashMap<int, GC::Ref<GC::Cell>> root_map;
    GC::RootHashTable<GC::Ptr<GC::Cell>> root_table;
    Vector<GC::RawPtr<GC::Cell>> raw_ptrs;
    Vector<GC::Weak<GC::Cell>> weak_refs;
    Vector<NonnullOwnPtr<Entry>> owned_entries;
    Vector<EntryWithoutGCPointers> entries_without_gc_pointers;
    Vector<int> integers;
    Vector<GC::Ref<GC::Cell>>& member_reference = state.cells;
    Span<GC::Ref<GC::Cell>> span = state.cells.span();
    GC::Ref<GC::Cell>* pointer_into_member = state.cells.data();
    static Vector<GC::Ref<GC::Cell>> static_local;
    for (auto cell : state.cells)
        (void)cell;
}

void range_over_temporary()
{
    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    for (auto cell : make_cells())
        (void)cell;
}
