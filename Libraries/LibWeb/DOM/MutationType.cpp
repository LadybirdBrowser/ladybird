/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/MutationType.h>

namespace Web::DOM::MutationType {

#define __ENUMERATE_MUTATION_TYPE(name) FlyString name;
ENUMERATE_MUTATION_TYPES
#undef __ENUMERATE_MUTATION_TYPE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_MUTATION_TYPE(name) name = #name##_fly_string;
    ENUMERATE_MUTATION_TYPES
#undef __ENUMERATE_MUTATION_TYPE

    s_initialized = true;
}

}
