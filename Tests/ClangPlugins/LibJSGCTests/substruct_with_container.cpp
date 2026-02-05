/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibJS/Runtime/Object.h>

// A substruct that contains GC pointers in a container but has no visit_edges method
struct SubStructWithVector {
    Vector<GC::Ptr<JS::Object>> m_objects;
};

struct SubStructWithHashMap {
    HashMap<int, GC::Ptr<JS::Object>> m_map;
};

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_vector_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithVector m_vector_substruct;

    // expected-error@+1 {{Member m_hashmap_substruct contains GC pointers but its type has no visit_edges method}}
    SubStructWithHashMap m_hashmap_substruct;
};
