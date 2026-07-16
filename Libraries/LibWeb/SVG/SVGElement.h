/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Weak.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/HTML/HTMLOrSVGOrMathMLElement.h>
#include <LibWeb/SVG/SVGAnimatedString.h>

namespace Web::SVG {

class WEB_API SVGElement
    : public DOM::Element
    , public HTML::GlobalEventHandlers
    , public HTML::HTMLOrSVGOrMathMLElement<SVGElement> {
    WEB_PLATFORM_OBJECT(SVGElement, DOM::Element);
    GC_DECLARE_ALLOCATOR(SVGElement);

public:
    virtual bool requires_svg_container() const override { return true; }

    GC::Ref<SVGAnimatedString> class_name();
    GC::Ptr<SVGSVGElement> owner_svg_element();
    GC::Ptr<SVGElement> viewport_element();

    bool should_include_in_accessibility_tree() const;
    virtual Optional<ARIA::Role> default_role() const override;

    GC::Ref<SVGAnimatedLength> fake_animated_length_fixme() const;
    GC::Ref<SVGAnimatedLength> svg_animated_length_for_property(CSS::PropertyID) const;

    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    void register_resource_box_referencing_element(Badge<Layout::TreeBuilder>, DOM::Element&);

protected:
    SVGElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual WebIDL::ExceptionOr<void> cloned(DOM::Node&, bool) const override;
    virtual void children_changed(ChildrenChangedMetadata const&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    MUST_UPCALL virtual void adjust_computed_style(CSS::ComputedProperties::Builder&) override;

    void update_use_elements_that_reference_this();
    void remove_from_use_element_that_reference_this();
    void mark_resource_box_referencing_elements_for_layout_tree_update();

private:
    // ^HTML::GlobalEventHandlers
    virtual GC::Ptr<DOM::EventTarget> global_event_handlers_to_event_target(Utf16FlyString const&) override { return *this; }

    virtual bool is_svg_element() const final { return true; }

    GC::Ptr<SVGAnimatedString> m_class_name_animated_string;

    // Elements whose layout subtrees contain a <mask>, <clipPath>, or <pattern> resource box built
    // from this element. Their subtrees must be rebuilt when this element is removed, since resource
    // boxes are not attached under this element's own layout position.
    Vector<GC::Weak<DOM::Element>> m_resource_box_referencing_elements;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGElement>() const { return is_svg_element(); }

}
