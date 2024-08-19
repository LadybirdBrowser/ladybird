/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGTransformList final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGTransformList, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGTransformList);

public:
    [[nodiscard]] static GC::Ref<SVGTransformList> create(JS::Realm& realm);
    virtual ~SVGTransformList() override;

    WebIDL::UnsignedLong length();
    WebIDL::UnsignedLong number_of_items();

    WebIDL::ExceptionOr<GC::Ref<SVGTransform>> get_item(WebIDL::UnsignedLong index);

    GC::Ref<SVGTransform> append_item(GC::Ref<SVGTransform> new_item);

private:
    SVGTransformList(JS::Realm& realm);

    virtual void initialize(JS::Realm& realm) override;
    virtual void visit_edges(Cell::Visitor& visitor) override;

    Vector<GC::Ref<SVGTransform>> m_transforms;
};

}
