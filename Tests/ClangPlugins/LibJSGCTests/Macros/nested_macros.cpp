/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>

// expected-error@+1 {{TestClass derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class TestClass : JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    struct NestedClassOk : JS::Cell {
        GC_CELL(NestedClassOk, JS::Cell);
    };

    // expected-error@+2 {{Expected record to have a JS_OBJECT macro invocation}}
    // expected-error@+1 {{NestedClassBad derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
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

    // expected-error@+2 {{Expected record to have a JS_OBJECT macro invocation}}
    // expected-error@+1 {{NestedClassBad derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
    struct NestedClassBad : JS::Object {
    };

    struct NestedClassNonCell {
    };
};
