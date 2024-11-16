/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibGC/ForeignCell.h>
#include <LibJS/Runtime/PrototypeObject.h>
#include <LibWeb/Bindings/PlatformObject.h>

// expected-error@+1 {{Expected record to have a GC_CELL macro invocation}}
class TestCellClass : JS::Cell {
};

// expected-error@+1 {{Expected record to have a FOREIGN_CELL macro invocation}}
class TestForeignCellClass : GC::ForeignCell {
};

// expected-error@+1 {{Expected record to have a JS_OBJECT macro invocation}}
class TestObjectClass : JS::Object {
};

// expected-error@+1 {{Expected record to have a JS_ENVIRONMENT macro invocation}}
class TestEnvironmentClass : JS::Environment {
};

// expected-error@+1 {{Expected record to have a JS_PROTOTYPE_OBJECT macro invocation}}
class TestPrototypeClass : JS::PrototypeObject<TestCellClass, TestCellClass> {
};

// expected-error@+1 {{Expected record to have a WEB_PLATFORM_OBJECT macro invocation}}
class TestPlatformClass : Web::Bindings::PlatformObject {
};
