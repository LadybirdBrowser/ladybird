/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>

// A substruct with visit_edges that doesn't visit all its members
struct IncompleteVisitEdges {
    // expected-error@+1 {{GC-allocated member is not visited in IncompleteVisitEdges::visit_edges}}
    GC::Ptr<GC::Cell> m_not_visited;
    GC::Ptr<GC::Cell> m_visited;

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(m_visited);
        // m_not_visited is not visited!
    }
};
