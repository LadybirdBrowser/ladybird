/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::MathML::TagNames {

#define ENUMERATE_MATHML_TAGS             \
    __ENUMERATE_MATHML_TAG(annotation)    \
    __ENUMERATE_MATHML_TAG(maction)       \
    __ENUMERATE_MATHML_TAG(malignmark)    \
    __ENUMERATE_MATHML_TAG(math)          \
    __ENUMERATE_MATHML_TAG(merror)        \
    __ENUMERATE_MATHML_TAG(mglyph)        \
    __ENUMERATE_MATHML_TAG(mfrac)         \
    __ENUMERATE_MATHML_TAG(mi)            \
    __ENUMERATE_MATHML_TAG(mmultiscripts) \
    __ENUMERATE_MATHML_TAG(mn)            \
    __ENUMERATE_MATHML_TAG(mo)            \
    __ENUMERATE_MATHML_TAG(mover)         \
    __ENUMERATE_MATHML_TAG(mpadded)       \
    __ENUMERATE_MATHML_TAG(mphantom)      \
    __ENUMERATE_MATHML_TAG(mprescripts)   \
    __ENUMERATE_MATHML_TAG(mroot)         \
    __ENUMERATE_MATHML_TAG(mrow)          \
    __ENUMERATE_MATHML_TAG(ms)            \
    __ENUMERATE_MATHML_TAG(mspace)        \
    __ENUMERATE_MATHML_TAG(msqrt)         \
    __ENUMERATE_MATHML_TAG(mstyle)        \
    __ENUMERATE_MATHML_TAG(msub)          \
    __ENUMERATE_MATHML_TAG(msubsup)       \
    __ENUMERATE_MATHML_TAG(msup)          \
    __ENUMERATE_MATHML_TAG(mtable)        \
    __ENUMERATE_MATHML_TAG(mtd)           \
    __ENUMERATE_MATHML_TAG(mtext)         \
    __ENUMERATE_MATHML_TAG(mtr)           \
    __ENUMERATE_MATHML_TAG(munder)        \
    __ENUMERATE_MATHML_TAG(munderover)    \
    __ENUMERATE_MATHML_TAG(semantics)

#define __ENUMERATE_MATHML_TAG(name) extern FlyString name;
ENUMERATE_MATHML_TAGS
#undef __ENUMERATE_MATHML_TAG

extern FlyString annotation_xml;

void initialize_strings();

}
