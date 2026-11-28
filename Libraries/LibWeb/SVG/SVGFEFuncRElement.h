/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEFuncRElement
class SVGFEFuncRElement final : public SVGComponentTransferFunctionElement {
    WEB_WRAPPABLE(SVGFEFuncRElement, SVGComponentTransferFunctionElement);

public:
    virtual ~SVGFEFuncRElement() override = default;

private:
    explicit SVGFEFuncRElement(DOM::RustMadePrefix);
};

}
