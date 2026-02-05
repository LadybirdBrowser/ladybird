/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::ContentSecurityPolicy::Directives::KeywordTrustedTypes {

// https://www.w3.org/TR/trusted-types/#tt-keyword
#define ENUMERATE_KEYWORD_TRUSTED_TYPES                                     \
    __ENUMERATE_KEYWORD_TRUSTED_TYPE(AllowDuplicates, "'allow-duplicates'") \
    __ENUMERATE_KEYWORD_TRUSTED_TYPE(None, "'none'")                        \
    __ENUMERATE_KEYWORD_TRUSTED_TYPE(WildCard, "*")

#define __ENUMERATE_KEYWORD_TRUSTED_TYPE(name, value) extern FlyString name;
ENUMERATE_KEYWORD_TRUSTED_TYPES
#undef __ENUMERATE_KEYWORD_TRUSTED_TYPE

}
