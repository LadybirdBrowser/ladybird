/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/Wrappable.h>

class TestCellClass : JS::Cell {
    GC_CELL(TestCellClass, JS::Cell);
};

class TestWrappableClass : Web::Bindings::Wrappable {
    WEB_NON_IDL_WRAPPABLE(TestWrappableClass, Web::Bindings::Wrappable);
};

// Nested classes
class Parent1 { };
class Parent2 : JS::Cell {
    GC_CELL(Parent2, JS::Cell);
};
class Parent3 { };
class Parent4 : public Parent2 {
    GC_CELL(Parent4, Parent2);
};

class NestedCellClass
    : Parent1
    , Parent3
    , Parent4 {
    GC_CELL(NestedCellClass, Parent4); // Not Parent2
};
