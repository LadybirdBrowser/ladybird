/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>

class Owned {
    AK_ALLOC_WITH_KMALLOC;

public:
    int value { 0 };
};

class Counted : public RefCounted<Counted> {
public:
    int value { 0 };
};

void test()
{
    // expected-error@+1 {{'NonnullOwnPtr<Owned>' is adopted without an allocator check. Use adopt_own(), adopt_ref() or make() instead}}
    auto owned = NonnullOwnPtr<Owned>(NonnullOwnPtr<Owned>::Adopt, *new Owned);

    // expected-error@+1 {{'OwnPtr<Owned>' is lifted without an allocator check. Use adopt_own_if_nonnull() instead}}
    auto lifted = OwnPtr<Owned>::lift(new Owned);

    // expected-error@+1 {{'NonnullRefPtr<Counted>' is adopted without an allocator check. Use adopt_own(), adopt_ref() or make() instead}}
    auto counted = NonnullRefPtr<Counted>(NonnullRefPtr<Counted>::Adopt, *new Counted);

    // expected-error@+1 {{'RefPtr<Counted>' is adopted without an allocator check. Use adopt_own(), adopt_ref() or make() instead}}
    auto maybe_counted = RefPtr<Counted>(RefPtr<Counted>::Adopt, *new Counted);
}
