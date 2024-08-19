/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1
// expected-no-diagnostics

#include <LibJS/Runtime/Object.h>

class NonCell {
    GC::Ptr<JS::Object> m_object;
};
