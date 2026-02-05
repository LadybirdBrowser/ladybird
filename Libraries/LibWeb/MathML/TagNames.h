/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::MathML::TagNames {

#define ENUMERATE_MATHML_TAGS                                \
    __ENUMERATE_MATHML_TAG(annotation, "annotation")         \
    __ENUMERATE_MATHML_TAG(annotation_xml, "annotation-xml") \
    __ENUMERATE_MATHML_TAG(maction, "maction")               \
    __ENUMERATE_MATHML_TAG(malignmark, "malignmark")         \
    __ENUMERATE_MATHML_TAG(math, "math")                     \
    __ENUMERATE_MATHML_TAG(merror, "merror")                 \
    __ENUMERATE_MATHML_TAG(mfrac, "mfrac")                   \
    __ENUMERATE_MATHML_TAG(mglyph, "mglyph")                 \
    __ENUMERATE_MATHML_TAG(mi, "mi")                         \
    __ENUMERATE_MATHML_TAG(mmultiscripts, "mmultiscripts")   \
    __ENUMERATE_MATHML_TAG(mn, "mn")                         \
    __ENUMERATE_MATHML_TAG(mo, "mo")                         \
    __ENUMERATE_MATHML_TAG(mover, "mover")                   \
    __ENUMERATE_MATHML_TAG(mpadded, "mpadded")               \
    __ENUMERATE_MATHML_TAG(mphantom, "mphantom")             \
    __ENUMERATE_MATHML_TAG(mprescripts, "mprescripts")       \
    __ENUMERATE_MATHML_TAG(mroot, "mroot")                   \
    __ENUMERATE_MATHML_TAG(mrow, "mrow")                     \
    __ENUMERATE_MATHML_TAG(ms, "ms")                         \
    __ENUMERATE_MATHML_TAG(mspace, "mspace")                 \
    __ENUMERATE_MATHML_TAG(msqrt, "msqrt")                   \
    __ENUMERATE_MATHML_TAG(mstyle, "mstyle")                 \
    __ENUMERATE_MATHML_TAG(msub, "msub")                     \
    __ENUMERATE_MATHML_TAG(msubsup, "msubsup")               \
    __ENUMERATE_MATHML_TAG(msup, "msup")                     \
    __ENUMERATE_MATHML_TAG(mtable, "mtable")                 \
    __ENUMERATE_MATHML_TAG(mtd, "mtd")                       \
    __ENUMERATE_MATHML_TAG(mtext, "mtext")                   \
    __ENUMERATE_MATHML_TAG(mtr, "mtr")                       \
    __ENUMERATE_MATHML_TAG(munder, "munder")                 \
    __ENUMERATE_MATHML_TAG(munderover, "munderover")         \
    __ENUMERATE_MATHML_TAG(semantics, "semantics")

#define __ENUMERATE_MATHML_TAG(name, tag) extern FlyString name;
ENUMERATE_MATHML_TAGS
#undef __ENUMERATE_MATHML_TAG

}
