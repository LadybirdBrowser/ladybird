/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGDescElement final : public SVGElement {
    WEB_WRAPPABLE(SVGDescElement, SVGElement);

private:
    explicit SVGDescElement(DOM::RustMadePrefix);
    virtual CSS::ElementBoxKind box_kind() const override;
};

}
