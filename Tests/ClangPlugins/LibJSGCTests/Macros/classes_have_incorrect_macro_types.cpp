/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/Wrappable.h>

class CellWithWrappableMacro : JS::Cell {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected GC_CELL}}
    WEB_NON_IDL_WRAPPABLE(CellWithWrappableMacro, JS::Cell);
};

class WrappableWithCellMacro : Web::Bindings::Wrappable {
    // expected-error@+1 {{Invalid GC-CELL-like macro invocation; expected WEB_WRAPPABLE}}
    GC_CELL(WrappableWithCellMacro, Web::Bindings::Wrappable);
};
