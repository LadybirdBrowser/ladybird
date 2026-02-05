/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEFuncGElement
class SVGFEFuncGElement final : public SVGComponentTransferFunctionElement {
    WEB_PLATFORM_OBJECT(SVGFEFuncGElement, SVGComponentTransferFunctionElement);
    GC_DECLARE_ALLOCATOR(SVGFEFuncGElement);

public:
    virtual ~SVGFEFuncGElement() override = default;

private:
    SVGFEFuncGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
