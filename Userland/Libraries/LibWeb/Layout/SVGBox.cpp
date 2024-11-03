/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/SVGBox.h>

namespace Web::Layout {

SVGBox::SVGBox(DOM::Document& document, SVG::SVGElement& element, CSS::StyleProperties style)
    : Box(document, &element, move(style))
{
}

}
