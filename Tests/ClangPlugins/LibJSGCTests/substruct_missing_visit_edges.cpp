/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>

// A substruct that contains GC pointers but has no visit_edges method.
// This is fine on its own (e.g. for stack use), but becomes an error when
// used as a Cell member.
struct SubStructWithoutVisitEdges {
    GC::Ptr<GC::Cell> m_object;
};

class TestClass : public GC::Cell {
    GC_CELL(TestClass, GC::Cell);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithoutVisitEdges m_substruct;
};
