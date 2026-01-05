/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>

// A substruct that contains GC pointers AND has a visit_edges method
struct SubStructWithVisitEdges {
    GC::Ptr<GC::Cell> m_object;

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_object);
    }
};

class TestClass : public GC::Cell {
    GC_CELL(TestClass, GC::Cell);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        m_substruct.visit_edges(visitor);
    }

    SubStructWithVisitEdges m_substruct;
};
