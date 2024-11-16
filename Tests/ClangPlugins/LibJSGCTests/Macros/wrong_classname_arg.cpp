/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibGC/ForeignCell.h>
#include <LibJS/Runtime/PrototypeObject.h>
#include <LibWeb/Bindings/PlatformObject.h>

// An incorrect first argument for JS_PROTOTYPE_OBJECT is a compile error, so that is not tested

class TestCellClass : JS::Cell {
    // expected-error@+1 {{Expected first argument of GC_CELL macro invocation to be TestCellClass}}
    GC_CELL(bad, JS::Cell);
};

class TestForeignCellClass : GC::ForeignCell {
    // expected-error@+1 {{Expected first argument of FOREIGN_CELL macro invocation to be TestForeignCellClass}}
    FOREIGN_CELL(bad, GC::ForeignCell);
};

class TestObjectClass : JS::Object {
    // expected-error@+1 {{Expected first argument of JS_OBJECT macro invocation to be TestObjectClass}}
    JS_OBJECT(bad, JS::Object);
};

class TestEnvironmentClass : JS::Environment {
    // expected-error@+1 {{Expected first argument of JS_ENVIRONMENT macro invocation to be TestEnvironmentClass}}
    JS_ENVIRONMENT(bad, JS::Environment);
};

class TestPlatformClass : Web::Bindings::PlatformObject {
    // expected-error@+1 {{Expected first argument of WEB_PLATFORM_OBJECT macro invocation to be TestPlatformClass}}
    WEB_PLATFORM_OBJECT(bad, Web::Bindings::PlatformObject);
};

struct Outer {
    struct Inner;
};

struct Outer::Inner : JS::Cell {
    // expected-error@+1 {{Expected first argument of GC_CELL macro invocation to be Outer::Inner}}
    GC_CELL(Inner, JS::Cell);
};
