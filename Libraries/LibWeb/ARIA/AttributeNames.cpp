/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/AttributeNames.h>

namespace Web::ARIA::AttributeNames {

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    Utf16FlyString const& name = *new Utf16FlyString(attribute##_utf16_fly_string);
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

}
