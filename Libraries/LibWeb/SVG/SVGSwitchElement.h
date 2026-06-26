/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/struct.html#SwitchElement
class SVGSwitchElement final : public SVGGraphicsElement {
    WEB_PLATFORM_OBJECT(SVGSwitchElement, SVGGraphicsElement);
    GC_DECLARE_ALLOCATOR(SVGSwitchElement);

public:
    virtual ~SVGSwitchElement() override;

    virtual RefPtr<Layout::Node> create_layout_node(CSS::ComputedProperties const&) override;

private:
    SVGSwitchElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_svg_switch_element() const override { return true; }
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGSwitchElement>() const { return is_svg_switch_element(); }

}
