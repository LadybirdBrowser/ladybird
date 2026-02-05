/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <LibJS/Runtime/Object.h>

// Inner struct containing a GC pointer with proper visit_edges
struct InnerStruct {
    GC::Ptr<JS::Object> m_inner_object;

    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_inner_object);
    }
};

// Outer struct containing the inner struct with proper visit_edges
struct OuterStruct {
    InnerStruct m_inner;

    void visit_edges(JS::Cell::Visitor& visitor)
    {
        m_inner.visit_edges(visitor);
    }
};

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        m_outer.visit_edges(visitor);
    }

    OuterStruct m_outer;
};
