/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/Names.h>

namespace Web::ContentSecurityPolicy::Directives::Names {

#define __ENUMERATE_DIRECTIVE_NAME(name, value) \
    Utf16FlyString const& name = *new Utf16FlyString(value##_utf16_fly_string);
ENUMERATE_DIRECTIVE_NAMES
#undef __ENUMERATE_DIRECTIVE_NAME

}
