/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>

namespace Web::SVG::AttributeNames {

#define __ENUMERATE_SVG_ATTRIBUTE(name) FlyString name;
ENUMERATE_SVG_ATTRIBUTES(__ENUMERATE_SVG_ATTRIBUTE)
#undef __ENUMERATE_SVG_ATTRIBUTE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_SVG_ATTRIBUTE(name) \
    name = #name##_fly_string;
    ENUMERATE_SVG_ATTRIBUTES(__ENUMERATE_SVG_ATTRIBUTE)
#undef __ENUMERATE_SVG_ATTRIBUTE

    // NOTE: Special cases for C++ keywords.
    class_ = "class"_fly_string;

    // NOTE: Special case for attributes with ':' in them.
    xlink_href = "xlink:href"_fly_string;

    s_initialized = true;
}

}
