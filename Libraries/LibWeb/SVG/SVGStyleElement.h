/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGStyleElement final
    : public SVGElement
    , public DOM::StyleElementBase {
    WEB_PLATFORM_OBJECT(SVGStyleElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGStyleElement);

public:
    virtual ~SVGStyleElement() override;

    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual bool contributes_a_script_blocking_style_sheet() const final;

private:
    SVGStyleElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Node
    virtual bool is_svg_style_element() const override { return true; }

    // ^DOM::StyleElementBase
    virtual Element& as_element() override { return *this; }
    virtual Element const& as_element() const override { return *this; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
