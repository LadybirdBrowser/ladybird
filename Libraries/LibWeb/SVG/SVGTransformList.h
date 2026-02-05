/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGTransformList final
    : public Bindings::PlatformObject
    , public SVGList<GC::Ref<SVGTransform>> {
    WEB_PLATFORM_OBJECT(SVGTransformList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGTransformList);

public:
    [[nodiscard]] static GC::Ref<SVGTransformList> create(JS::Realm& realm, Vector<GC::Ref<SVGTransform>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGTransformList> create(JS::Realm& realm, ReadOnlyList);
    virtual ~SVGTransformList() override = default;

private:
    SVGTransformList(JS::Realm&, Vector<GC::Ref<SVGTransform>>, ReadOnlyList);
    SVGTransformList(JS::Realm&, ReadOnlyList);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;
};

}
