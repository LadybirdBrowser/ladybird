/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG/interact.html#InterfaceSVGScriptElement
class SVGScriptElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes> {
    WEB_PLATFORM_OBJECT(SVGScriptElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGScriptElement);

public:
    void process_the_script_element();

    void set_source_line_number(Badge<HTML::HTMLParser>, size_t source_line_number) { m_source_line_number = source_line_number; }

protected:
    SVGScriptElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

private:
    virtual bool is_svg_script_element() const final { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    bool m_already_processed { false };

    JS::GCPtr<HTML::ClassicScript> m_script;

    size_t m_source_line_number { 1 };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGScriptElement>() const { return is_svg_script_element(); }

}
