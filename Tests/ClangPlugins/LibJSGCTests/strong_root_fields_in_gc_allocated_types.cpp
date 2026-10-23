/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -fplugin-arg-libjs_gc_scanner-detect-invalid-function-members -c %s -o %t 2>&1

#include <LibGC/Root.h>
#include <LibJS/Heap/Cell.h>

class CellClass : JS::Cell {
    GC_CELL(CellClass, JS::Cell);

    // expected-warning@+1 {{Types inheriting from GC::Cell should not have GC::Root fields}}
    GC::Root<JS::Cell> m_handle;
};

class NonCellClass {
    GC::Root<JS::Cell> m_handle;
};
