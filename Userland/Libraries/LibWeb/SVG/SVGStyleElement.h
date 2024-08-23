/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleElementUtils.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGStyleElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGStyleElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGStyleElement);

public:
    virtual ~SVGStyleElement() override;

    virtual void children_changed() override;
    virtual void inserted() override;
    virtual void removed_from(Node*) override;

    CSS::CSSStyleSheet* sheet();
    CSS::CSSStyleSheet const* sheet() const;

private:
    SVGStyleElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Node
    virtual bool is_svg_style_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // The semantics and processing of a ‘style’ and its attributes must be the same as is defined for the HTML ‘style’ element.
    DOM::StyleElementUtils m_style_element_utils;
};

}
