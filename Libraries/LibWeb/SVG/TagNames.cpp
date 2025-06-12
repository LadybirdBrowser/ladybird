/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/TagNames.h>

namespace Web::SVG::TagNames {

#define __ENUMERATE_SVG_TAG(name) \
    FlyString name = #name##_fly_string;
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

}
