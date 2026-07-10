/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MathML/TagNames.h>

namespace Web::MathML::TagNames {

#define __ENUMERATE_MATHML_TAG(name, tag) \
    Utf16FlyString const& name = *new Utf16FlyString(tag##_utf16_fly_string);
ENUMERATE_MATHML_TAGS
#undef __ENUMERATE_MATHML_TAG

}
