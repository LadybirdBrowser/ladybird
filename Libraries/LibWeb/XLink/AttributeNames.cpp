/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/XLink/AttributeNames.h>

namespace Web::XLink::AttributeNames {

#define __ENUMERATE_XLINK_ATTRIBUTE(name) \
    Utf16FlyString name = #name##_utf16_fly_string;
ENUMERATE_XLINK_ATTRIBUTES
#undef __ENUMERATE_XLINK_ATTRIBUTE

}
