/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/XLink/AttributeNames.h>

namespace Web::XLink::AttributeNames {

#define __ENUMERATE_XLINK_ATTRIBUTE(name) FlyString name;
ENUMERATE_XLINK_ATTRIBUTES(__ENUMERATE_XLINK_ATTRIBUTE)
#undef __ENUMERATE_XLINK_ATTRIBUTE

void initialize_strings()
{
    static bool s_initialized = false;
    VERIFY(!s_initialized);

#define __ENUMERATE_XLINK_ATTRIBUTE(name) \
    name = #name##_fly_string;
    ENUMERATE_XLINK_ATTRIBUTES(__ENUMERATE_XLINK_ATTRIBUTE)
#undef __ENUMERATE_XLINK_ATTRIBUTE

    s_initialized = true;
}

}
