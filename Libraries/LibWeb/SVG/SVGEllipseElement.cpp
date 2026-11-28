/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Window.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGEllipseElement.h>

namespace Web::SVG {

SVGEllipseElement::SVGEllipseElement(DOM::RustMadePrefix prefix)
    : SVGGeometryElement(prefix)
{
}

}
