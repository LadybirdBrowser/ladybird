/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGAElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGAElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAElement);

SVGAElement::SVGAElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGAElement::~SVGAElement() = default;

void SVGAElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAElement);
}

void SVGAElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void SVGAElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href) {
        invalidate_style(
            DOM::StyleInvalidationReason::HTMLHyperlinkElementHrefChange,
            {
                { .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::AnyLink },
                { .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::Link },
                { .type = CSS::InvalidationSet::Property::Type::PseudoClass, .value = CSS::PseudoClass::LocalLink },
            },
            {});
    }
    if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 SVGAElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

// https://svgwg.org/svg2-draft/linking.html#__svg__SVGAElement__relList
GC::Ref<DOM::DOMTokenList> SVGAElement::rel_list()
{
    // The relList IDL attribute reflects the ‘rel’ content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

GC::Ptr<Layout::Node> SVGAElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGGraphicsBox>(document(), *this, move(style));
}

}
