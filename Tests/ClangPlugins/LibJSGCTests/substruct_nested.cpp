/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>

// Inner struct containing a GC pointer (fine on its own for stack use)
struct InnerStruct {
    GC::Ptr<JS::Object> m_inner_object;
};

// Outer struct containing the inner struct (fine on its own for stack use)
struct OuterStruct {
    InnerStruct m_inner;
};

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_outer contains GC pointers but its type has no visit_edges method}}
    OuterStruct m_outer;
};
