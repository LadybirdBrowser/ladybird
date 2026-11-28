/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/SVGGElement.h>

namespace Web::SVG {

SVGGElement::SVGGElement(DOM::RustMadePrefix prefix)
    : SVGGraphicsElement(prefix)
{
}

CSS::ElementBoxKind SVGGElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgGraphics;
}

}
