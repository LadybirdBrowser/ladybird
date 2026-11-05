/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLOrSVGScriptElement.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGURIReference.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG/interact.html#InterfaceSVGScriptElement
class SVGScriptElement
    : public SVGElement
    , public SVGURIReferenceMixin<SupportsXLinkHref::Yes>
    , public HTML::HTMLOrSVGScriptElement {
    WEB_WRAPPABLE(SVGScriptElement, SVGElement);
    GC_DECLARE_ALLOCATOR(SVGScriptElement);

public:
    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void post_connection() override;
    virtual bool has_post_connection_steps() const override { return true; }
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual WebIDL::ExceptionOr<void> cloned(Node&, bool) const override;

protected:
    SVGScriptElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual bool is_svg_script_element() const final { return true; }

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void adopted_from(DOM::Document&) override;

    // ^HTMLOrSVGScriptElement
    virtual DOM::Element& as_element() override { return *this; }
    virtual DOM::Element const& as_element() const override { return *this; }
    virtual bool has_source_attribute() const override;
    virtual Utf16String source_attribute_value() override;
    virtual Optional<Utf16String> language_attribute_value() const override { return {}; }
    virtual Optional<Utf16String> charset_attribute_value() const override { return {}; }
    virtual Optional<Utf16String> event_attribute_value() const override { return {}; }
    virtual Optional<Utf16String> for_attribute_value() const override { return {}; }
    virtual bool has_defer_attribute() const override { return false; }
    virtual bool has_nomodule_attribute() const override { return false; }
    virtual HTML::ScriptElementFetchAttributes fetch_attributes() const override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGScriptElement>() const { return is_svg_script_element(); }

}
