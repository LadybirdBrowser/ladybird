/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/RefCounted.h>
#include <AK/kmalloc.h>

class RogueDelete : public RefCounted<RogueDelete> {
public:
    // expected-error@+1 {{'RogueDelete' inherits kmalloc allocation but declares its own operator delete}}
    static void operator delete(void* pointer) noexcept
    {
        kfree(pointer);
    }
};

class RogueNew : public RefCounted<RogueNew> {
public:
    // expected-error@+1 {{'RogueNew' inherits kmalloc allocation but declares its own operator new}}
    static void* operator new(size_t size)
    {
        return kmalloc(size);
    }
};
