/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/SVGTransformList.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/SVG/SVGList.h>
#include <LibWeb/SVG/SVGTransform.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/single-page.html#coords-InterfaceSVGTransformList
class SVGTransformList final
    : public Bindings::Wrappable
    , public SVGList<GC::Ref<SVGTransform>> {
    WEB_WRAPPABLE(SVGTransformList, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(SVGTransformList);

public:
    [[nodiscard]] static GC::Ref<SVGTransformList> create(Vector<GC::Ref<SVGTransform>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGTransformList> create(ReadOnlyList);
    virtual ~SVGTransformList() override = default;

private:
    SVGTransformList(Vector<GC::Ref<SVGTransform>>, ReadOnlyList);
    explicit SVGTransformList(ReadOnlyList);

    virtual Optional<JS::Value> item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(JS::Realm&, u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(JS::Realm&, u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_indexed_property(JS::Realm&, u32, JS::Value) override;
    virtual void visit_edges(Visitor&) override;
};

}
