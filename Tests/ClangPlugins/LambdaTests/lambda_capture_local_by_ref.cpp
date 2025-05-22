/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t 2>&1

#include <AK/Function.h>

void take_fn(Function<void()>) { }
void take_fn_escaping(ESCAPING Function<void()>) { }

void test()
{
    // expected-note@+1 {{Annotate the variable declaration with IGNORE_USE_IN_ESCAPING_LAMBDA if it outlives the lambda}}
    int a = 0;

    take_fn([&a] {
        (void)a;
    });

    // expected-error@+1 {{Variable with local storage is captured by reference in a lambda marked ESCAPING}}
    take_fn_escaping([&a] {
        (void)a;
    });
}
