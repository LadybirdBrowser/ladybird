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
#include <LibWeb/SVG/SVGLength.h>

#define REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(attribute_name, getter_name, directionality, default_value) \
    GC::Ref<SVGAnimatedLength> getter_name() { return svg_animated_length_for_attribute(AttributeNames::attribute_name, SVGLength::Directionality::directionality, default_value); }

#define REFLECT_ANIMATED_LENGTH_ATTRIBUTE(attribute_name, directionality, default_value) \
    REFLECT_ANIMATED_LENGTH_ATTRIBUTE_WITH_GETTER(attribute_name, attribute_name, directionality, default_value)

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

    GC::Ref<SVGAnimatedLength> svg_animated_length_for_attribute(Utf16FlyString const&, SVGLength::Directionality, NonnullRefPtr<CSS::StyleValue const>&& default_value);

    virtual bool is_presentational_hint(Utf16FlyString const&) const final override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const final override;

    void register_resource_box_referencing_element(Badge<Layout::LayoutTreeBuilderAccess>, DOM::Element&);

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

    // Many reflecting attributes are marked as SameObject so we cache the objects we create here.
    HashMap<Utf16FlyString, GC::Ref<JS::Object>> m_reflected_attribute_cache;

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
