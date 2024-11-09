/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/TagNames.h>

namespace Web::SVG::TagNames {

#define __ENUMERATE_SVG_TAG(name) FlyString name;
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_SVG_TAG(name) name = #name##_fly_string;
    ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

    s_initialized = true;
}

}
