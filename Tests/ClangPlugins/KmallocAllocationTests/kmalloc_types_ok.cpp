/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <string>

class Tagged {
    AK_ALLOC_WITH_KMALLOC;

public:
    int value { 0 };
};

class Counted : public RefCounted<Counted> {
public:
    int value { 0 };
};

class ReTagged : public RefCounted<ReTagged> {
    AK_ALLOC_WITH_KMALLOC_PARTITION(HeapPartition::String);

public:
    int value { 0 };
};

void test()
{
    auto* tagged = new Tagged;
    delete tagged;

    auto* nothrow_tagged = new (nothrow) Tagged;
    delete nothrow_tagged;

    auto owned = make<Tagged>();
    auto adopted = adopt_own(*new Tagged);
    auto maybe_owned = adopt_own_if_nonnull(new (nothrow) Tagged);
    auto counted = make_ref_counted<Counted>();
    auto adopted_counted = adopt_ref(*new Counted);
    auto maybe_counted = adopt_ref_if_nonnull(new (nothrow) Counted);
    auto re_tagged = make_ref_counted<ReTagged>();

    struct Local {
        AK_ALLOC_WITH_KMALLOC;
        int value { 0 };
    };
    auto local = make<Local>();

    auto* system_type = new std::string("system allocator");
    delete system_type;
}
