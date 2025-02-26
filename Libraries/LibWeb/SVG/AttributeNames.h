/*
 * Copyright (c) 2021-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::SVG::AttributeNames {

#define ENUMERATE_SVG_ATTRIBUTES                                          \
    __ENUMERATE_SVG_ATTRIBUTE(attributeName, "attributeName")             \
    __ENUMERATE_SVG_ATTRIBUTE(attributeType, "attributeType")             \
    __ENUMERATE_SVG_ATTRIBUTE(baseFrequency, "baseFrequency")             \
    __ENUMERATE_SVG_ATTRIBUTE(baseProfile, "baseProfile")                 \
    __ENUMERATE_SVG_ATTRIBUTE(calcMode, "calcMode")                       \
    __ENUMERATE_SVG_ATTRIBUTE(class_, "class")                            \
    __ENUMERATE_SVG_ATTRIBUTE(clipPathUnits, "clipPathUnits")             \
    __ENUMERATE_SVG_ATTRIBUTE(contentScriptType, "contentScriptType")     \
    __ENUMERATE_SVG_ATTRIBUTE(contentStyleType, "contentStyleType")       \
    __ENUMERATE_SVG_ATTRIBUTE(cx, "cx")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(cy, "cy")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(diffuseConstant, "diffuseConstant")         \
    __ENUMERATE_SVG_ATTRIBUTE(dx, "dx")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(dy, "dy")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(edgeMode, "edgeMode")                       \
    __ENUMERATE_SVG_ATTRIBUTE(filterUnits, "filterUnits")                 \
    __ENUMERATE_SVG_ATTRIBUTE(fr, "fr")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(fx, "fx")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(fy, "fy")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(glyphRef, "glyphRef")                       \
    __ENUMERATE_SVG_ATTRIBUTE(gradientTransform, "gradientTransform")     \
    __ENUMERATE_SVG_ATTRIBUTE(gradientUnits, "gradientUnits")             \
    __ENUMERATE_SVG_ATTRIBUTE(height, "height")                           \
    __ENUMERATE_SVG_ATTRIBUTE(href, "href")                               \
    __ENUMERATE_SVG_ATTRIBUTE(kernelMatrix, "kernelMatrix")               \
    __ENUMERATE_SVG_ATTRIBUTE(kernelUnitLength, "kernelUnitLength")       \
    __ENUMERATE_SVG_ATTRIBUTE(keyPoints, "keyPoints")                     \
    __ENUMERATE_SVG_ATTRIBUTE(keySplines, "keySplines")                   \
    __ENUMERATE_SVG_ATTRIBUTE(keyTimes, "keyTimes")                       \
    __ENUMERATE_SVG_ATTRIBUTE(lengthAdjust, "lengthAdjust")               \
    __ENUMERATE_SVG_ATTRIBUTE(limitingConeAngle, "limitingConeAngle")     \
    __ENUMERATE_SVG_ATTRIBUTE(markerHeight, "markerHeight")               \
    __ENUMERATE_SVG_ATTRIBUTE(markerUnits, "markerUnits")                 \
    __ENUMERATE_SVG_ATTRIBUTE(markerWidth, "markerWidth")                 \
    __ENUMERATE_SVG_ATTRIBUTE(maskContentUnits, "maskContentUnits")       \
    __ENUMERATE_SVG_ATTRIBUTE(maskUnits, "maskUnits")                     \
    __ENUMERATE_SVG_ATTRIBUTE(numOctaves, "numOctaves")                   \
    __ENUMERATE_SVG_ATTRIBUTE(offset, "offset")                           \
    __ENUMERATE_SVG_ATTRIBUTE(opacity, "opacity")                         \
    __ENUMERATE_SVG_ATTRIBUTE(pathLength, "pathLength")                   \
    __ENUMERATE_SVG_ATTRIBUTE(patternContentUnits, "patternContentUnits") \
    __ENUMERATE_SVG_ATTRIBUTE(patternTransform, "patternTransform")       \
    __ENUMERATE_SVG_ATTRIBUTE(patternUnits, "patternUnits")               \
    __ENUMERATE_SVG_ATTRIBUTE(points, "points")                           \
    __ENUMERATE_SVG_ATTRIBUTE(pointsAtX, "pointsAtX")                     \
    __ENUMERATE_SVG_ATTRIBUTE(pointsAtY, "pointsAtY")                     \
    __ENUMERATE_SVG_ATTRIBUTE(pointsAtZ, "pointsAtZ")                     \
    __ENUMERATE_SVG_ATTRIBUTE(preserveAlpha, "preserveAlpha")             \
    __ENUMERATE_SVG_ATTRIBUTE(preserveAspectRatio, "preserveAspectRatio") \
    __ENUMERATE_SVG_ATTRIBUTE(primitiveUnits, "primitiveUnits")           \
    __ENUMERATE_SVG_ATTRIBUTE(r, "r")                                     \
    __ENUMERATE_SVG_ATTRIBUTE(refX, "refX")                               \
    __ENUMERATE_SVG_ATTRIBUTE(refY, "refY")                               \
    __ENUMERATE_SVG_ATTRIBUTE(repeatCount, "repeatCount")                 \
    __ENUMERATE_SVG_ATTRIBUTE(repeatDur, "repeatDur")                     \
    __ENUMERATE_SVG_ATTRIBUTE(requiredExtensions, "requiredExtensions")   \
    __ENUMERATE_SVG_ATTRIBUTE(requiredFeatures, "requiredFeatures")       \
    __ENUMERATE_SVG_ATTRIBUTE(rx, "rx")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(ry, "ry")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(specularConstant, "specularConstant")       \
    __ENUMERATE_SVG_ATTRIBUTE(specularExponent, "specularExponent")       \
    __ENUMERATE_SVG_ATTRIBUTE(spreadMethod, "spreadMethod")               \
    __ENUMERATE_SVG_ATTRIBUTE(startOffset, "startOffset")                 \
    __ENUMERATE_SVG_ATTRIBUTE(stdDeviation, "stdDeviation")               \
    __ENUMERATE_SVG_ATTRIBUTE(stitchTiles, "stitchTiles")                 \
    __ENUMERATE_SVG_ATTRIBUTE(surfaceScale, "surfaceScale")               \
    __ENUMERATE_SVG_ATTRIBUTE(systemLanguage, "systemLanguage")           \
    __ENUMERATE_SVG_ATTRIBUTE(tableValues, "tableValues")                 \
    __ENUMERATE_SVG_ATTRIBUTE(targetX, "targetX")                         \
    __ENUMERATE_SVG_ATTRIBUTE(targetY, "targetY")                         \
    __ENUMERATE_SVG_ATTRIBUTE(textLength, "textLength")                   \
    __ENUMERATE_SVG_ATTRIBUTE(type, "type")                               \
    __ENUMERATE_SVG_ATTRIBUTE(version, "version")                         \
    __ENUMERATE_SVG_ATTRIBUTE(viewBox, "viewBox")                         \
    __ENUMERATE_SVG_ATTRIBUTE(viewTarget, "viewTarget")                   \
    __ENUMERATE_SVG_ATTRIBUTE(width, "width")                             \
    __ENUMERATE_SVG_ATTRIBUTE(x, "x")                                     \
    __ENUMERATE_SVG_ATTRIBUTE(x1, "x1")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(x2, "x2")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(xChannelSelector, "xChannelSelector")       \
    __ENUMERATE_SVG_ATTRIBUTE(xlink_href, "xlink:href")                   \
    __ENUMERATE_SVG_ATTRIBUTE(y, "y")                                     \
    __ENUMERATE_SVG_ATTRIBUTE(y1, "y1")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(y2, "y2")                                   \
    __ENUMERATE_SVG_ATTRIBUTE(yChannelSelector, "yChannelSelector")       \
    __ENUMERATE_SVG_ATTRIBUTE(zoomAndPan, "zoomAndPan")

#define __ENUMERATE_SVG_ATTRIBUTE(name, attribute) extern FlyString name;
ENUMERATE_SVG_ATTRIBUTES
#undef __ENUMERATE_SVG_ATTRIBUTE

}
