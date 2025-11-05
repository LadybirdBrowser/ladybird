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
    WEB_PLATFORM_OBJECT(SVGFEFuncRElement, SVGComponentTransferFunctionElement);
    GC_DECLARE_ALLOCATOR(SVGFEFuncRElement);

public:
    virtual ~SVGFEFuncRElement() override = default;

private:
    SVGFEFuncRElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
