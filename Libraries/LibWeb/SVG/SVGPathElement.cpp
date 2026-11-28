/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGPathElement.h>

namespace Web::SVG {

SVGPathElement::SVGPathElement(DOM::RustMadePrefix prefix)
    : SVGGeometryElement(prefix)
{
}

}
