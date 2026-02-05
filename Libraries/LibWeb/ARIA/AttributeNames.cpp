/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/AttributeNames.h>

namespace Web::ARIA::AttributeNames {

#define __ENUMERATE_ARIA_ATTRIBUTE(name, attribute) \
    FlyString name = attribute##_fly_string;
ENUMERATE_ARIA_ATTRIBUTES
#undef __ENUMERATE_ARIA_ATTRIBUTE

}
