/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <AK/NonnullOwnPtr.h>
#include <AK/kmalloc.h>

class Pooled {
public:
    static NonnullOwnPtr<Pooled> create()
    {
        auto* memory = kmalloc(sizeof(Pooled));
        return adopt_own(*::new (memory) Pooled);
    }

    void operator delete(void* pointer)
    {
        kfree(pointer);
    }

    int value { 0 };
};

template<>
inline constexpr bool AllocatedWithCustomAllocator<Pooled> = true;

void test()
{
    auto pooled = Pooled::create();
    auto* plain = new Pooled;
    delete plain;
}
