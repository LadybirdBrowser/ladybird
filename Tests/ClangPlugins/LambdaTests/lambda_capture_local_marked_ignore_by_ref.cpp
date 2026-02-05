/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1
// expected-no-diagnostics

#include <AK/Function.h>

void take_fn(ESCAPING Function<void()>) { }

void test()
{
    IGNORE_USE_IN_ESCAPING_LAMBDA int a = 0;

    take_fn([&a] {
        (void)a;
    });
}
