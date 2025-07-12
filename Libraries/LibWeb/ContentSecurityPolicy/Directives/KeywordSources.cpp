/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordSources {

#define __ENUMERATE_KEYWORD_SOURCE(name, value) \
    FlyString name = value##_fly_string;
ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

}
