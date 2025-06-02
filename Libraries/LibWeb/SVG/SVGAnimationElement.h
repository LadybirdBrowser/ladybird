/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/SVGAnimationElementPrototype.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGAnimationElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGAnimationElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGAnimationElement);

private:
    SVGAnimationElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
