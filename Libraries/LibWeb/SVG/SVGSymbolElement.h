/*
 * Copyright (c) 2023, Preston Taylor <95388976+PrestonLTaylor@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

class SVGSymbolElement final : public SVGGraphicsElement
    , public SVGFitToViewBox {
    WEB_PLATFORM_OBJECT(SVGSymbolElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGSymbolElement);

public:
    virtual ~SVGSymbolElement() override = default;

    virtual void adjust_computed_style(CSS::ComputedProperties::Builder&) override;

private:
    virtual bool is_svg_symbol_element() const final { return true; }

    SVGSymbolElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual RefPtr<Layout::Node> create_layout_node(NonnullRefPtr<CSS::ComputedValues const>) override;

    bool is_direct_child_of_use_shadow_tree() const;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGSymbolElement>() const { return is_svg_symbol_element(); }

}
