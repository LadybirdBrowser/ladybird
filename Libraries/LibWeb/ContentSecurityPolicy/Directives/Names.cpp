/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>

namespace Web::ContentSecurityPolicy::Directives::Names {

#define __ENUMERATE_DIRECTIVE_NAME(name, value) FlyString name;
ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_DIRECTIVE_NAME(name, value) \
    name = value##_fly_string;
    ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME

    s_initialized = true;
}

}
