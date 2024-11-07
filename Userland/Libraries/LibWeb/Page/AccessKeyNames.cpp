/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Page/AccessKeyNames.h>

namespace Web::HTML::AccessKeyNames {

#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) FlyString name; /* NOLINT(misc-confusable-identifiers) */
ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#ifdef AK_OS_MACOS
#    define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
        name = maclabel##_fly_string;
    ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#    undef __ENUMERATE_ACCESS_KEY
#else
#    define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) \
        name = label##_fly_string;
    ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#    undef __ENUMERATE_ACCESS_KEY
#endif

    s_initialized = true;
}

}
