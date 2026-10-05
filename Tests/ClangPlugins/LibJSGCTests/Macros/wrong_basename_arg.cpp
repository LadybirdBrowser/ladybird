/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Heap/Cell.h>

// The only way to have an incorrect basename is if the class is deeply nested, and the base name
// refers to a parent class

class ParentCell : JS::Cell {
    GC_CELL(ParentCell, JS::Cell);
};

class TestClass : ParentCell {
    // expected-error@+1 {{Expected second argument of GC_CELL macro invocation to be ParentCell}}
    GC_CELL(TestClass, JS::Cell);
};

// Basename must exactly match the argument
namespace JS {

class TestClass : ::ParentCell {
    // expected-error@+1 {{Expected second argument of GC_CELL macro invocation to be ::ParentCell}}
    GC_CELL(TestClass, ParentCell);
};

}

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
    // expected-error@+1 {{Expected second argument of GC_CELL macro invocation to be Parent4}}
    GC_CELL(NestedCellClass, Parent2);
};
