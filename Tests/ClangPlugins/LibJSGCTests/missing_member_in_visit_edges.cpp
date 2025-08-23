/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>

class ForwardDeclaredObject;

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
    }

    // expected-error@+1 {{GC-allocated member is not visited in TestClass::visit_edges}}
    GC::Ptr<JS::Object> m_object;

    // expected-error@+1 {{GC-allocated member is not visited in TestClass::visit_edges}}
    JS::Value m_value;

    // expected-error@+1 {{GC-allocated member is not visited in TestClass::visit_edges}}
    GC::Ptr<ForwardDeclaredObject> m_forward_declared_object;
};
