/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/OwnPtr.h>

// expected-note@+1 3 {{'Untagged' is defined here}}
struct Untagged {
    int value { 0 };
};

void test()
{
    // expected-error@+1 {{'Untagged' is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it}}
    auto* untagged = new Untagged;
    delete untagged;

    // expected-error@+1 {{'Untagged' is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it}}
    auto* nothrow_untagged = new (nothrow) Untagged;
    delete nothrow_untagged;

    // expected-error@+1 {{'Untagged' is allocated with the system allocator. Add AK_ALLOC_WITH_KMALLOC to the class, or specialize AllocatedWithSystemAllocator or AllocatedWithCustomAllocator for it}}
    auto* untagged_array = new Untagged[4];
    delete[] untagged_array;
}
