/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/struct.html#InterfaceSVGMetadataElement
class SVGMetadataElement final : public SVGElement {
    WEB_WRAPPABLE(SVGMetadataElement, SVGElement);

private:
    explicit SVGMetadataElement(DOM::RustMadePrefix);
    virtual CSS::ElementBoxKind box_kind() const override;
};

}
