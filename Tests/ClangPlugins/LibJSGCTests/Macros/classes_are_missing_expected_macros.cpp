/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/Wrappable.h>

// expected-error@+1 {{Expected record to have a GC_CELL macro invocation}}
class TestCellClass : JS::Cell {
};

// expected-error@+1 {{Expected record to have a WEB_WRAPPABLE macro invocation}}
class TestWrappableClass : Web::Bindings::Wrappable {
};
