/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/Path.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::SVG {

class SVGPathElement final : public SVGGeometryElement {
    WEB_WRAPPABLE(SVGPathElement, SVGGeometryElement);

public:
    virtual ~SVGPathElement() override = default;

private:
    explicit SVGPathElement(DOM::RustMadePrefix);
};

}
