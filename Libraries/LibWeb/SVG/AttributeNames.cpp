/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>

namespace Web::SVG::AttributeNames {

#define __ENUMERATE_SVG_ATTRIBUTE(name, attribute) \
    FlyString name = attribute##_fly_string;
ENUMERATE_SVG_ATTRIBUTES
#undef __ENUMERATE_SVG_ATTRIBUTE

}
