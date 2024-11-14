/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibJS/Runtime/Object.h>

// expected-error@+1 {{GC::Cell-inheriting class TestClass contains a GC-allocated member 'm_cell' but has no visit_edges method}}
class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    GC::Ptr<JS::Object> m_cell;
};
