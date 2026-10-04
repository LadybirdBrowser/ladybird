/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrototypeObject.h>
#include <LibWeb/Bindings/Wrappable.h>

// Note: It's pretty hard to have the incorrect type in a JS::PrototypeObject, since the base name would
//       have a comma in it, and wouldn't be passable as the basename without a typedef.

class CellWithObjectMacro : JS::Cell {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected GC_CELL}}
    JS_OBJECT(CellWithObjectMacro, JS::Cell);
};

class CellWithEnvironmentMacro : JS::Cell {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected GC_CELL}}
    JS_ENVIRONMENT(CellWithEnvironmentMacro, JS::Cell);
};

// expected-error@+1 {{ObjectWithCellMacro derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class ObjectWithCellMacro : JS::Object {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected JS_OBJECT}}
    GC_CELL(ObjectWithCellMacro, JS::Object);
};

// expected-error@+1 {{ObjectWithEnvironmentMacro derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class ObjectWithEnvironmentMacro : JS::Object {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected JS_OBJECT}}
    JS_ENVIRONMENT(ObjectWithEnvironmentMacro, JS::Object);
};

class WrappableWithCellMacro : Web::Bindings::Wrappable {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected WEB_WRAPPABLE}}
    GC_CELL(WrappableWithCellMacro, Web::Bindings::Wrappable);
};

// expected-error@+1 {{ObjectWithWrappableMacro derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class ObjectWithWrappableMacro : JS::Object {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected JS_OBJECT}}
    WEB_NON_IDL_WRAPPABLE(ObjectWithWrappableMacro, JS::Object);
};

// JS_PROTOTYPE_OBJECT can only be used in the JS namespace
namespace JS {

class CellWithPrototypeMacro : Cell {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected GC_CELL}}
    JS_PROTOTYPE_OBJECT(CellWithPrototypeMacro, Cell, Cell);
};

// expected-error@+1 {{ObjectWithPrototypeMacro derives from the engine type JS::Object, which only LibJS may subclass; use a host class instead}}
class ObjectWithPrototypeMacro : Object {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected JS_OBJECT}}
    JS_PROTOTYPE_OBJECT(ObjectWithPrototypeMacro, Object, Object);
};

}
