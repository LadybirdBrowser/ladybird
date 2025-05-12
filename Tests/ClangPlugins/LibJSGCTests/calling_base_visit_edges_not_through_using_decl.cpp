/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    // expected-error@+1 {{Missing call to Base::visit_edges}}
    virtual void visit_edges(Visitor& visitor) override
    {
        JS::Object::visit_edges(visitor);
    }
};
