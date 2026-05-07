/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/RootHashMap.h>
#include <LibGC/RootVector.h>

// RootVector with a non-GC element type should fail.
void test_root_vector_non_gc_type(GC::Heap& heap)
{
    // expected-error@*{{RootVector element type must be convertible to Cell const* or derive from NanBoxedValue}}
    // expected-note@+1 {{in instantiation of member function}}
    GC::RootVector<int> bad_vector(heap);
}

// RootHashMap where neither key nor value is a GC type should fail.
void test_root_hash_map_non_gc_types(GC::Heap& heap)
{
    // expected-error@*{{RootHashMap requires at least one of key or value types to be convertible to Cell const* or derive from NanBoxedValue}}
    // expected-note@+1 {{in instantiation of member function}}
    GC::RootHashMap<int, int> bad_map(heap);
}
