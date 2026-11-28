/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGSwitchElement.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/SVG/SVGSwitchElement.h>

namespace Web::SVG {

SVGSwitchElement::SVGSwitchElement(DOM::RustMadePrefix prefix)
    : SVGGraphicsElement(prefix)
{
}

SVGSwitchElement::~SVGSwitchElement() = default;

CSS::ElementBoxKind SVGSwitchElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgGraphics;
}

}
