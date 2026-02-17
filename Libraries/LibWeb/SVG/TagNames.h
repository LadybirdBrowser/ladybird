/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::SVG::TagNames {

#define ENUMERATE_SVG_TAGS                   \
    __ENUMERATE_SVG_TAG(a)                   \
    __ENUMERATE_SVG_TAG(circle)              \
    __ENUMERATE_SVG_TAG(clipPath)            \
    __ENUMERATE_SVG_TAG(defs)                \
    __ENUMERATE_SVG_TAG(desc)                \
    __ENUMERATE_SVG_TAG(ellipse)             \
    __ENUMERATE_SVG_TAG(feBlend)             \
    __ENUMERATE_SVG_TAG(feColorMatrix)       \
    __ENUMERATE_SVG_TAG(feComponentTransfer) \
    __ENUMERATE_SVG_TAG(feComposite)         \
    __ENUMERATE_SVG_TAG(feDropShadow)        \
    __ENUMERATE_SVG_TAG(feFlood)             \
    __ENUMERATE_SVG_TAG(feFuncA)             \
    __ENUMERATE_SVG_TAG(feFuncB)             \
    __ENUMERATE_SVG_TAG(feFuncG)             \
    __ENUMERATE_SVG_TAG(feFuncR)             \
    __ENUMERATE_SVG_TAG(feGaussianBlur)      \
    __ENUMERATE_SVG_TAG(feImage)             \
    __ENUMERATE_SVG_TAG(feMerge)             \
    __ENUMERATE_SVG_TAG(feMergeNode)         \
    __ENUMERATE_SVG_TAG(feMorphology)        \
    __ENUMERATE_SVG_TAG(feOffset)            \
    __ENUMERATE_SVG_TAG(feTurbulence)        \
    __ENUMERATE_SVG_TAG(filter)              \
    __ENUMERATE_SVG_TAG(foreignObject)       \
    __ENUMERATE_SVG_TAG(g)                   \
    __ENUMERATE_SVG_TAG(image)               \
    __ENUMERATE_SVG_TAG(line)                \
    __ENUMERATE_SVG_TAG(linearGradient)      \
    __ENUMERATE_SVG_TAG(mask)                \
    __ENUMERATE_SVG_TAG(metadata)            \
    __ENUMERATE_SVG_TAG(path)                \
    __ENUMERATE_SVG_TAG(pattern)             \
    __ENUMERATE_SVG_TAG(polygon)             \
    __ENUMERATE_SVG_TAG(polyline)            \
    __ENUMERATE_SVG_TAG(radialGradient)      \
    __ENUMERATE_SVG_TAG(rect)                \
    __ENUMERATE_SVG_TAG(script)              \
    __ENUMERATE_SVG_TAG(stop)                \
    __ENUMERATE_SVG_TAG(style)               \
    __ENUMERATE_SVG_TAG(svg)                 \
    __ENUMERATE_SVG_TAG(symbol)              \
    __ENUMERATE_SVG_TAG(text)                \
    __ENUMERATE_SVG_TAG(textPath)            \
    __ENUMERATE_SVG_TAG(title)               \
    __ENUMERATE_SVG_TAG(tspan)               \
    __ENUMERATE_SVG_TAG(use)                 \
    __ENUMERATE_SVG_TAG(view)

#define __ENUMERATE_SVG_TAG(name) extern FlyString name;
ENUMERATE_SVG_TAGS
#undef __ENUMERATE_SVG_TAG

}
