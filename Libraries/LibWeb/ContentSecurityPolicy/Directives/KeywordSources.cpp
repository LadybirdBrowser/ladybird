/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/KeywordSources.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordSources {

#define __ENUMERATE_KEYWORD_SOURCE(name, value) \
    Utf16FlyString const& name = *new Utf16FlyString(value##_utf16_fly_string);
ENUMERATE_KEYWORD_SOURCES
#undef __ENUMERATE_KEYWORD_SOURCE

}
