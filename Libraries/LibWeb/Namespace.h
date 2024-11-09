/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FlyString.h>

namespace Web::Namespace {

#define ENUMERATE_NAMESPACES                                            \
    __ENUMERATE_NAMESPACE(HTML, "http://www.w3.org/1999/xhtml")         \
    __ENUMERATE_NAMESPACE(MathML, "http://www.w3.org/1998/Math/MathML") \
    __ENUMERATE_NAMESPACE(SVG, "http://www.w3.org/2000/svg")            \
    __ENUMERATE_NAMESPACE(XLink, "http://www.w3.org/1999/xlink")        \
    __ENUMERATE_NAMESPACE(XML, "http://www.w3.org/XML/1998/namespace")  \
    __ENUMERATE_NAMESPACE(XMLNS, "http://www.w3.org/2000/xmlns/")

#define __ENUMERATE_NAMESPACE(name, namespace_) extern FlyString name;
ENUMERATE_NAMESPACES
#undef __ENUMERATE_NAMESPACE

void initialize_strings();

}
