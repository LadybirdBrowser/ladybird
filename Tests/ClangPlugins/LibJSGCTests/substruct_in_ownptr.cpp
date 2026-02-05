/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/OwnPtr.h>
#include <LibJS/Runtime/Object.h>

// A substruct that contains GC pointers but has no visit_edges method.
// This is fine on its own (stack use), but becomes an error when stored
// in OwnPtr (malloc heap, not scanned conservatively).
struct SubStructInOwnPtr {
    GC::Ptr<JS::Object> m_object;
};

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{Member m_substruct contains GC pointers but its type has no visit_edges method}}
    OwnPtr<SubStructInOwnPtr> m_substruct;
};
