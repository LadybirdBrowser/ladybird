/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibWeb/Bindings/Wrappable.h>

// expected-error@+1 {{Expected record to have a GC_CELL macro invocation}}
class TestCellClass : JS::Cell {
};

// expected-error@+2 {{Expected record to have a JS_OBJECT macro invocation}}
// expected-error@+1 {{TestObjectClass derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class TestObjectClass : JS::Object {
};

// expected-error@+2 {{Expected record to have a JS_ENVIRONMENT macro invocation}}
// expected-error@+1 {{TestEnvironmentClass derives from the engine type JS::Environment, which only LibJS may subclass; use a host class instead}}
class TestEnvironmentClass : JS::Environment {
};

// expected-error@+2 {{Expected record to have a JS_PROTOTYPE_OBJECT macro invocation}}
// expected-error@+1 {{TestPrototypeClass derives from the engine type JS::PrototypeObject, which only LibJS may subclass; use a host class instead}}
class TestPrototypeClass : JS::PrototypeObject<TestCellClass, TestCellClass> {
};

// expected-error@+1 {{Expected record to have a WEB_WRAPPABLE macro invocation}}
class TestWrappableClass : Web::Bindings::Wrappable {
};
