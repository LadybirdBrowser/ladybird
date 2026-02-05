/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>

// A substruct that contains a JS::Value but has no visit_edges method
// (fine on stack, but not as a Cell member)
struct SubStructWithValue {
    JS::Value m_value;
};

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithValue m_substruct;
};
