/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Namespace.h>

namespace Web::Namespace {

#define __ENUMERATE_NAMESPACE(name, namespace_) FlyString name;
ENUMERATE_NAMESPACES
#undef __ENUMERATE_NAMESPACE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_NAMESPACE(name, namespace_) \
    name = namespace_##_fly_string;
    ENUMERATE_NAMESPACES
#undef __ENUMERATE_NAMESPACE

    s_initialized = true;
}

}
