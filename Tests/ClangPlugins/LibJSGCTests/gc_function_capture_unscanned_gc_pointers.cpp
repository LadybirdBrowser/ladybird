/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>

struct Entry {
    // expected-note@+1 5 {{Member cell contains GC pointers}}
    GC::Ref<GC::Cell> cell;
};

struct RootedEntry {
    GC::Root<GC::Cell> cell;
};

struct SharedEntry : RefCounted<SharedEntry> {
    // expected-note@+1 {{Member cell contains GC pointers}}
    GC::Ref<GC::Cell> cell;
};

struct State {
    NonnullOwnPtr<Entry> entry;
};

struct Params {
    virtual ~Params() = default;
    virtual void visit_edges(GC::Cell::Visitor&) { }
};

struct ParamsWithCell : Params {
    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        Params::visit_edges(visitor);
        visitor.visit(cell);
    }

    // expected-note@+1 {{Member cell contains GC pointers}}
    GC::Ref<GC::Cell> cell;
};

struct TemplateParamsBase {
    virtual ~TemplateParamsBase() = default;
    virtual void visit_edges(GC::Cell::Visitor&) { }
};

template<typename T>
struct TemplateParams : TemplateParamsBase {
    virtual void visit_edges(GC::Cell::Visitor& visitor) override
    {
        TemplateParamsBase::visit_edges(visitor);
        visitor.visit(cell);
    }

    // expected-note@+1 {{Member cell contains GC pointers}}
    T cell;
};

static_assert(sizeof(TemplateParams<GC::Ref<GC::Cell>>) > 0);

void unscanned_captures(GC::Heap& heap, Vector<GC::Ref<GC::Cell>> const& cells, NonnullOwnPtr<Entry> entry, NonnullOwnPtr<Entry> other_entry, NonnullOwnPtr<Entry> third_entry, State state, NonnullOwnPtr<Params> params, NonnullOwnPtr<Vector<NonnullOwnPtr<Entry>>> nested_entries, NonnullOwnPtr<TemplateParamsBase> template_params, NonnullOwnPtr<Entry> parenthesized_entry, NonnullRefPtr<SharedEntry> shared_entry)
{
    // expected-error@+1 {{Captured 'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [cells] { (void)cells; });

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<Entry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [entry = move(entry)] { (void)entry; });

    // expected-error@+1 {{Captured 'NonnullOwnPtr<Entry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [state = move(state)] { (void)state; });

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<Params>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [params = move(params)] { (void)params; });

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<AK::Vector<AK::NonnullOwnPtr<Entry>>>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [nested_entries = move(nested_entries)] { (void)nested_entries; });

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<TemplateParamsBase>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [template_params = move(template_params)] { (void)template_params; });

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<Entry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, ([parenthesized_entry = move(parenthesized_entry)] { (void)parenthesized_entry; }));

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<Entry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    auto lambda = [other_entry = move(other_entry)] { (void)other_entry; };
    (void)GC::create_function(heap, move(lambda));

    // expected-error@+1 {{Captured 'AK::NonnullOwnPtr<Entry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::Function<void()>::create(heap, [third_entry = move(third_entry)] { (void)third_entry; });

    // expected-error@+1 {{'Vector<GC::Ref<GC::Cell>>' keeps GC pointers in heap memory that is not scanned for roots}}
    (void)GC::create_function(heap, [copy = cells] { (void)copy; });

    // expected-error@+1 {{Captured 'NonnullRefPtr<SharedEntry>' keeps GC pointers in heap memory that GC::Function does not scan}}
    (void)GC::create_function(heap, [shared_entry] { (void)shared_entry; });
}

void scanned_captures(GC::Heap& heap, GC::Ref<GC::Cell> cell, GC::RootVector<GC::Ref<GC::Cell>> const& rooted_cells, NonnullOwnPtr<RootedEntry> rooted_entry, Entry const& entry)
{
    (void)GC::create_function(heap, [cell, rooted_cells, rooted_entry = move(rooted_entry), entry] {
        (void)cell;
        (void)rooted_cells;
        (void)rooted_entry;
        (void)entry;
    });
}
