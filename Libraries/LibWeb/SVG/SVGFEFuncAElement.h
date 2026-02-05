/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/filter-effects-1/#InterfaceSVGFEFuncAElement
class SVGFEFuncAElement final : public SVGComponentTransferFunctionElement {
    WEB_PLATFORM_OBJECT(SVGFEFuncAElement, SVGComponentTransferFunctionElement);
    GC_DECLARE_ALLOCATOR(SVGFEFuncAElement);

public:
    virtual ~SVGFEFuncAElement() override = default;

private:
    SVGFEFuncAElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
