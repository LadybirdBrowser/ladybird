/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ContentSecurityPolicy/Directives/KeywordTrustedTypes.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordTrustedTypes {

#define __ENUMERATE_KEYWORD_TRUSTED_TYPE(name, value) \
    FlyString name = value##_fly_string;
ENUMERATE_KEYWORD_TRUSTED_TYPES
#undef __ENUMERATE_KEYWORD_TRUSTED_TYPE

}
