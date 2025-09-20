/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Export.h>

namespace Web::MathML::AttributeNames {

#define ENUMERATE_MATHML_ATTRIBUTES                                \
    __ENUMERATE_MATHML_ATTRIBUTE(autofocus, "autofocus")           \
    __ENUMERATE_MATHML_ATTRIBUTE(depth, "depth")                   \
    __ENUMERATE_MATHML_ATTRIBUTE(dir, "dir")                       \
    __ENUMERATE_MATHML_ATTRIBUTE(displaystyle, "displaystyle")     \
    __ENUMERATE_MATHML_ATTRIBUTE(height, "height")                 \
    __ENUMERATE_MATHML_ATTRIBUTE(mathbackground, "mathbackground") \
    __ENUMERATE_MATHML_ATTRIBUTE(mathcolor, "mathcolor")           \
    __ENUMERATE_MATHML_ATTRIBUTE(mathsize, "mathsize")             \
    __ENUMERATE_MATHML_ATTRIBUTE(mathvariant, "mathvariant")       \
    __ENUMERATE_MATHML_ATTRIBUTE(scriptlevel, "scriptlevel")       \
    __ENUMERATE_MATHML_ATTRIBUTE(width, "width")

#define __ENUMERATE_MATHML_ATTRIBUTE(name, attribute) extern WEB_API FlyString name;
ENUMERATE_MATHML_ATTRIBUTES
#undef __ENUMERATE_MATHML_ATTRIBUTE

}
