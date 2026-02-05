/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -Xclang -verify %plugin_opts% -c %s -o %t  2>&1

#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>

struct NotACell { };

class TestClass {
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::Ptr<NotACell> m_member_1;
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::Ref<NotACell> m_member_2;
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::RawPtr<NotACell> m_member_3;
};
