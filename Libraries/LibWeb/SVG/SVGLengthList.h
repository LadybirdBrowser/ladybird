/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/SVGLengthList.h>
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
    [[nodiscard]] static GC::Ref<SVGLengthList> create(JS::Realm& realm, Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    [[nodiscard]] static GC::Ref<SVGLengthList> create(JS::Realm& realm, ReadOnlyList);
    virtual ~SVGLengthList() override = default;

private:
    SVGLengthList(JS::Realm&, Vector<GC::Ref<SVGLength>>, ReadOnlyList);
    SVGLengthList(JS::Realm&, ReadOnlyList);

    virtual JS::Realm& svg_list_realm() const override { return realm(); }
    virtual Optional<JS::Value> item_value(JS::Realm& realm, size_t index) const override;
    virtual WebIDL::ExceptionOr<void> set_value_of_new_indexed_property(u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_existing_indexed_property(u32, JS::Value) override;
    virtual WebIDL::ExceptionOr<void> set_value_of_indexed_property(u32, JS::Value) override;
    virtual void visit_edges(Visitor&) override;
};

}
