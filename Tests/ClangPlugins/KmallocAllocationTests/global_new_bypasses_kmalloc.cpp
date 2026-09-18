/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/RefCounted.h>

class Counted : public RefCounted<Counted> {
public:
    int value { 0 };
};

void test()
{
    // expected-error@+1 {{'Counted' allocates with kmalloc, but this new-expression bypasses its operator new}}
    auto* counted = ::new Counted;
    delete counted;

    // expected-error@+1 {{'Counted' allocates with kmalloc, but this new-expression bypasses its operator new}}
    auto* counted_array = new Counted[2];
    delete[] counted_array;
}
