/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGLengthList
class SVGLengthList final
    : public Bindings::Wrappable
    , public SVGList<GC::Ref<SVGLength>> {
    WEB_WRAPPABLE(SVGLengthList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SVGLengthList);

public:
    [[nodiscard]] static GC::Ref<SVGLengthList> create(Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGLengthList> create(ReadOnlyList);
    virtual ~SVGLengthList() override = default;

private:
    SVGLengthList(Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    explicit SVGLengthList(ReadOnlyList);

    virtual void visit_edges(Visitor&) override;
};

}
