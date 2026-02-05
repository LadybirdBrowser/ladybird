/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <LibGC/Cell.h>
#include <LibJS/Runtime/Object.h>

class TestClass : public JS::Object {
    JS_OBJECT(TestClass, JS::Object);

    GC::RawPtr<JS::Object> m_object;
};
