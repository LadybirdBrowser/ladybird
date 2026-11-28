/*
 * Copyright (c) 2022, Simon Danner <danner.simon@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/Box.h>
#include <LibWeb/SVG/SVGDefsElement.h>

namespace Web::SVG {

SVGDefsElement::SVGDefsElement(DOM::RustMadePrefix prefix)
    : SVGGraphicsElement(prefix)
{
}

SVGDefsElement::~SVGDefsElement()
{
}

}
