/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::SVG::TagNames {

#define ENUMERATE_SVG_TAGS                                          \
    __ENUMERATE_SVG_TAG(a, "a")                                     \
    __ENUMERATE_SVG_TAG(circle, "circle")                           \
    __ENUMERATE_SVG_TAG(clipPath, "clipPath")                       \
    __ENUMERATE_SVG_TAG(defs, "defs")                               \
    __ENUMERATE_SVG_TAG(desc, "desc")                               \
    __ENUMERATE_SVG_TAG(ellipse, "ellipse")                         \
    __ENUMERATE_SVG_TAG(feBlend, "feBlend")                         \
    __ENUMERATE_SVG_TAG(feColorMatrix, "feColorMatrix")             \
    __ENUMERATE_SVG_TAG(feComponentTransfer, "feComponentTransfer") \
    __ENUMERATE_SVG_TAG(feComposite, "feComposite")                 \
    __ENUMERATE_SVG_TAG(feDisplacementMap, "feDisplacementMap")     \
    __ENUMERATE_SVG_TAG(feDropShadow, "feDropShadow")               \
    __ENUMERATE_SVG_TAG(feFlood, "feFlood")                         \
    __ENUMERATE_SVG_TAG(feFuncA, "feFuncA")                         \
    __ENUMERATE_SVG_TAG(feFuncB, "feFuncB")                         \
    __ENUMERATE_SVG_TAG(feFuncG, "feFuncG")                         \
    __ENUMERATE_SVG_TAG(feFuncR, "feFuncR")                         \
    __ENUMERATE_SVG_TAG(feGaussianBlur, "feGaussianBlur")           \
    __ENUMERATE_SVG_TAG(feImage, "feImage")                         \
    __ENUMERATE_SVG_TAG(feMerge, "feMerge")                         \
    __ENUMERATE_SVG_TAG(feMergeNode, "feMergeNode")                 \
    __ENUMERATE_SVG_TAG(feMorphology, "feMorphology")               \
    __ENUMERATE_SVG_TAG(feOffset, "feOffset")                       \
    __ENUMERATE_SVG_TAG(feTurbulence, "feTurbulence")               \
    __ENUMERATE_SVG_TAG(filter, "filter")                           \
    __ENUMERATE_SVG_TAG(foreignObject, "foreignObject")             \
    __ENUMERATE_SVG_TAG(g, "g")                                     \
    __ENUMERATE_SVG_TAG(image, "image")                             \
    __ENUMERATE_SVG_TAG(line, "line")                               \
    __ENUMERATE_SVG_TAG(linearGradient, "linearGradient")           \
    __ENUMERATE_SVG_TAG(mask, "mask")                               \
    __ENUMERATE_SVG_TAG(metadata, "metadata")                       \
    __ENUMERATE_SVG_TAG(path, "path")                               \
    __ENUMERATE_SVG_TAG(pattern, "pattern")                         \
    __ENUMERATE_SVG_TAG(polygon, "polygon")                         \
    __ENUMERATE_SVG_TAG(polyline, "polyline")                       \
    __ENUMERATE_SVG_TAG(radialGradient, "radialGradient")           \
    __ENUMERATE_SVG_TAG(rect, "rect")                               \
    __ENUMERATE_SVG_TAG(script, "script")                           \
    __ENUMERATE_SVG_TAG(stop, "stop")                               \
    __ENUMERATE_SVG_TAG(style, "style")                             \
    __ENUMERATE_SVG_TAG(svg, "svg")                                 \
    __ENUMERATE_SVG_TAG(switch_, "switch")                          \
    __ENUMERATE_SVG_TAG(symbol, "symbol")                           \
    __ENUMERATE_SVG_TAG(text, "text")                               \
    __ENUMERATE_SVG_TAG(textPath, "textPath")                       \
    __ENUMERATE_SVG_TAG(title, "title")                             \
    __ENUMERATE_SVG_TAG(tspan, "tspan")                             \
    __ENUMERATE_SVG_TAG(use, "use")                                 \
    __ENUMERATE_SVG_TAG(view, "view")

#define __ENUMERATE_SVG_TAG(name, tag) extern FlyString const& name;
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

}
