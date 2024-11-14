/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1
// FIXME: Un-XFAIL this when re-enabling the plugin option in the Tests CMakeLists.txt
// XFAIL: true

#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/Handle.h>

class CellClass : JS::Cell {
    GC_CELL(CellClass, JS::Cell);

    // expected-warning@+1 {{Types inheriting from GC::Cell should not have GC::Root fields}}
    GC::Root<JS::Cell> m_handle;
};

class NonCellClass {
    GC::Root<JS::Cell> m_handle;
};
