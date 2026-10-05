/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/Wrappable.h>

class TestCellClass : JS::Cell {
    // expected-error@+1 {{Expected first argument of GC_CELL macro invocation to be TestCellClass}}
    GC_CELL(bad, JS::Cell);
};

class TestWrappableClass : Web::Bindings::Wrappable {
    // expected-error@+1 {{Expected first argument of WEB_WRAPPABLE macro invocation to be TestWrappableClass}}
    WEB_NON_IDL_WRAPPABLE(bad, Web::Bindings::Wrappable);
};

struct Outer {
    struct Inner;
};

struct Outer::Inner : JS::Cell {
    // expected-error@+1 {{Expected first argument of GC_CELL macro invocation to be Outer::Inner}}
    GC_CELL(Inner, JS::Cell);
};
