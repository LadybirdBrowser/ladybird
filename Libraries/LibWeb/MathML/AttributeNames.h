/*
 * Copyright (c) 2025, Lorenz Ackermann, <me@lorenzackermann.xyz>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Export.h>

namespace Web::MathML::AttributeNames {

#define ENUMERATE_MATHML_ATTRIBUTES                                \
    __ENUMERATE_MATHML_ATTRIBUTE(accent, "accent")                 \
    __ENUMERATE_MATHML_ATTRIBUTE(accentunder, "accentunder")       \
    __ENUMERATE_MATHML_ATTRIBUTE(autofocus, "autofocus")           \
    __ENUMERATE_MATHML_ATTRIBUTE(depth, "depth")                   \
    __ENUMERATE_MATHML_ATTRIBUTE(dir, "dir")                       \
    __ENUMERATE_MATHML_ATTRIBUTE(displaystyle, "displaystyle")     \
    __ENUMERATE_MATHML_ATTRIBUTE(form, "form")                     \
    __ENUMERATE_MATHML_ATTRIBUTE(height, "height")                 \
    __ENUMERATE_MATHML_ATTRIBUTE(href, "href")                     \
    __ENUMERATE_MATHML_ATTRIBUTE(largeop, "largeop")               \
    __ENUMERATE_MATHML_ATTRIBUTE(linethickness, "linethickness")   \
    __ENUMERATE_MATHML_ATTRIBUTE(lspace, "lspace")                 \
    __ENUMERATE_MATHML_ATTRIBUTE(mathbackground, "mathbackground") \
    __ENUMERATE_MATHML_ATTRIBUTE(mathcolor, "mathcolor")           \
    __ENUMERATE_MATHML_ATTRIBUTE(mathsize, "mathsize")             \
    __ENUMERATE_MATHML_ATTRIBUTE(mathvariant, "mathvariant")       \
    __ENUMERATE_MATHML_ATTRIBUTE(maxsize, "maxsize")               \
    __ENUMERATE_MATHML_ATTRIBUTE(minsize, "minsize")               \
    __ENUMERATE_MATHML_ATTRIBUTE(movablelimits, "movablelimits")   \
    __ENUMERATE_MATHML_ATTRIBUTE(rspace, "rspace")                 \
    __ENUMERATE_MATHML_ATTRIBUTE(scriptlevel, "scriptlevel")       \
    __ENUMERATE_MATHML_ATTRIBUTE(stretchy, "stretchy")             \
    __ENUMERATE_MATHML_ATTRIBUTE(symmetric, "symmetric")           \
    __ENUMERATE_MATHML_ATTRIBUTE(voffset, "voffset")               \
    __ENUMERATE_MATHML_ATTRIBUTE(width, "width")

#define __ENUMERATE_MATHML_ATTRIBUTE(name, attribute) extern WEB_API Utf16FlyString const& name;
ENUMERATE_MATHML_ATTRIBUTES
#undef __ENUMERATE_MATHML_ATTRIBUTE

}
