/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/TagNames.h>

namespace Web::SVG::TagNames {

#define __ENUMERATE_SVG_TAG(name, tag) \
    Utf16FlyString const& name = *new Utf16FlyString(tag##_utf16_fly_string);
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

#define __ENUMERATE_SVG_TAG(name, tag) \
    extern "C" FlatPtr const ladybird_svg_tag_name_##name = name.raw_identity();
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

}
