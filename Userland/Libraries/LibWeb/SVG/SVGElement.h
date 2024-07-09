/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::SVG {

class SVGElement : public DOM::Element {
    WEB_PLATFORM_OBJECT(SVGElement, DOM::Element);

public:
    virtual bool requires_svg_container() const override { return true; }

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override;

    virtual void children_changed() override;
    virtual void inserted() override;
    virtual void removed_from(Node*) override;

    [[nodiscard]] JS::NonnullGCPtr<HTML::DOMStringMap> dataset();

    void focus();
    void blur();

protected:
    SVGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    void update_use_elements_that_reference_this();
    void remove_from_use_element_that_reference_this();

    JS::GCPtr<HTML::DOMStringMap> m_dataset;

    JS::NonnullGCPtr<SVGAnimatedLength> svg_animated_length_for_property(CSS::PropertyID) const;

private:
    virtual bool is_svg_element() const final { return true; }
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGElement>() const { return is_svg_element(); }

}
