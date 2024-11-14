/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// RUN: %clang++ -cc1 -verify %plugin_opts% %s 2>&1

#include <LibGC/MarkedVector.h>
#include <LibGC/Ptr.h>

struct NotACell { };

class TestClass {
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::Ptr<NotACell> m_member_1;
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::Ref<NotACell> m_member_2;
    // expected-error@+1 {{Specialization type must inherit from GC::Cell}}
    GC::RawPtr<NotACell> m_member_3;
};
