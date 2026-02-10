/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGAnimatedInteger.h>
#include <LibWeb/SVG/SVGAnimatedNumber.h>
#include <LibWeb/SVG/SVGAnimatedString.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGFilterPrimitiveStandardAttributes.h>

namespace Web::SVG {

class SVGFETurbulenceElement final
    : public SVGElement
    , public SVGFilterPrimitiveStandardAttributes<SVGFETurbulenceElement> {
    WEB_PLATFORM_OBJECT(SVGFETurbulenceElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGFETurbulenceElement);

public:
    enum class TurbulenceType {
        Unknown = 0,
        FractalNoise = 1,
        Turbulence = 2,
    };

    enum class StitchType {
        Unknown = 0,
        Stitch = 1,
        NoStitch = 2,
    };

    virtual ~SVGFETurbulenceElement() override = default;

    GC::Ref<SVGAnimatedNumber> base_frequency_x();
    GC::Ref<SVGAnimatedNumber> base_frequency_y();
    GC::Ref<SVGAnimatedInteger> num_octaves();
    GC::Ref<SVGAnimatedNumber> seed();
    GC::Ref<SVGAnimatedEnumeration> stitch_tiles();
    GC::Ref<SVGAnimatedEnumeration> type();

protected:
    SVGFETurbulenceElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<SVGAnimatedNumber> m_base_frequency_x;
    GC::Ptr<SVGAnimatedNumber> m_base_frequency_y;
    GC::Ptr<SVGAnimatedInteger> m_num_octaves;
    GC::Ptr<SVGAnimatedNumber> m_seed;
    GC::Ptr<SVGAnimatedEnumeration> m_stitch_tiles;
    GC::Ptr<SVGAnimatedEnumeration> m_type;
};

}
