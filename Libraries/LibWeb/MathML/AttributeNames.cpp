/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MathML/AttributeNames.h>

namespace Web::MathML::AttributeNames {

#define __ENUMERATE_MATHML_ATTRIBUTE(name, attribute) \
    Utf16FlyString const& name = *new Utf16FlyString(attribute##_utf16_fly_string);
ENUMERATE_MATHML_ATTRIBUTES
#undef __ENUMERATE_MATHML_ATTRIBUTE

}
