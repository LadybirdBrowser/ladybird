/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>

// A substruct that contains a raw pointer to a Cell type (fine on stack)
struct SubStructWithRawPointer {
    // expected-error@+1 {{pointer to GC::Cell type should be wrapped in GC::Ptr}}
    GC::Cell* m_raw_ptr;
};

// A substruct that contains a raw reference to a Cell type (fine on stack)
struct SubStructWithRawReference {
    // expected-error@+1 {{reference to GC::Cell type should be wrapped in GC::Ref}}
    GC::Cell& m_raw_ref;
};

class TestClass : public GC::Cell {
    GC_CELL(TestClass, GC::Cell);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_ptr_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithRawPointer m_ptr_substruct;

    // expected-error@+1 {{Member m_ref_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithRawReference m_ref_substruct;
};
