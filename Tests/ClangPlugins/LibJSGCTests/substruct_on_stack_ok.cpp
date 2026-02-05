/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>

// A substruct that contains GC pointers but has no visit_edges method.
// This is fine because it's only used on the stack (conservative scanning).
struct StackOnlySubStruct {
    GC::Ptr<GC::Cell> m_object;
};

void some_function(GC::Cell& cell)
{
    // Using the substruct on the stack is fine
    StackOnlySubStruct s;
    s.m_object = &cell;
}
