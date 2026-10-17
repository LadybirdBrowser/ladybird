/*
 * Copyright (c) 2026, Timothy Flynn <trflynn89@pm.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

// expected-note@+1 {{'Untagged' is defined here}}
struct Untagged {
};

// Allocations in decltype operands are unevaluated and must not trigger allocator diagnostics.
// expected-warning@+1 {{expression with side effects has no effect in an unevaluated context}}
using Pointer = decltype(new Untagged);

template<typename T>
using PointerTo = decltype(new T);

static_assert(__is_same(PointerTo<Untagged>, Pointer));

void test()
{
    // Traversing decltype must not suppress diagnostics for subsequent evaluated allocations.
    Pointer pointer;
    // expected-error@+1 {{'Untagged' is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it}}
    pointer = new Untagged;
    delete pointer;
}
