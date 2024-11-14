/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibJS/Runtime/Object.h>

class TestClass : JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    struct NestedClassOk : JS::Cell {
        GC_CELL(NestedClassOk, JS::Cell);
    };

    // expected-error@+1 {{Expected record to have a JS_OBJECT macro invocation}}
    struct NestedClassBad : JS::Object {
    };

    struct NestedClassNonCell {
    };
};

// Same test, but the parent object is not a cell
class TestClass2 {
    struct NestedClassOk : JS::Cell {
        GC_CELL(NestedClassOk, JS::Cell);
    };

    // expected-error@+1 {{Expected record to have a JS_OBJECT macro invocation}}
    struct NestedClassBad : JS::Object {
    };

    struct NestedClassNonCell {
    };
};
