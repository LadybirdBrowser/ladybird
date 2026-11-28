/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/SVG/SVGMetadataElement.h>

namespace Web::SVG {

SVGMetadataElement::SVGMetadataElement(DOM::RustMadePrefix prefix)
    : SVGElement(prefix)
{
}

CSS::ElementBoxKind SVGMetadataElement::box_kind() const
{
    return CSS::ElementBoxKind::NoBox;
}

}
