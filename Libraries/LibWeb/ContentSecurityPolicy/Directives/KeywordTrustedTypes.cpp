/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/KeywordTrustedTypes.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordTrustedTypes {

#define __ENUMERATE_KEYWORD_TRUSTED_TYPE(name, value) \
    Utf16FlyString const& name = *new Utf16FlyString(value##_utf16_fly_string);
ENUMERATE_KEYWORD_TRUSTED_TYPES
#undef __ENUMERATE_KEYWORD_TRUSTED_TYPE

}
