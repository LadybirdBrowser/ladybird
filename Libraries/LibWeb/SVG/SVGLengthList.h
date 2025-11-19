/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGList.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGLengthList
class SVGLengthList final
    : public Bindings::PlatformObject
    , public SVGList<GC::Ref<SVGLength>> {
    WEB_PLATFORM_OBJECT(SVGLengthList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGLengthList);

public:
    [[nodiscard]] static GC::Ref<SVGLengthList> create(JS::Realm& realm, Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGLengthList> create(JS::Realm& realm, ReadOnlyList);
    virtual ~SVGLengthList() override = default;

private:
    SVGLengthList(JS::Realm&, Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    SVGLengthList(JS::Realm&, ReadOnlyList);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;
};

}
