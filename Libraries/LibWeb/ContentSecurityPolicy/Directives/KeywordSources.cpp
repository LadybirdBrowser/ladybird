/*
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordSources {

#define __ENUMERATE_KEYWORD_SOURCE(name, value) FlyString name;
ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_KEYWORD_SOURCE(name, value) \
    name = value##_fly_string;
    ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

    s_initialized = true;
}

}
