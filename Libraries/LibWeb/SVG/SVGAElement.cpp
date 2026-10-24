/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/Invalidation/LinkInvalidator.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAElement);

SVGAElement::SVGAElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGAElement::~SVGAElement() = default;

void SVGAElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_rel_list);
    visitor.visit(m_target);
}

void SVGAElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::href && (!namespace_.has_value() || namespace_ == Namespace::XLink))
        href_content_attribute_changed();
    if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.has_value() ? value->utf16_view() : u""sv);
    }
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 SVGAElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

// https://svgwg.org/svg2-draft/interact.html#Focus
bool SVGAElement::is_focusable() const
{
    // In addition, all ‘a’ elements that are valid links are focusable, and their tabindex focus flag must be set
    // unless the user agent normally provides an alternative method of keyboard traversal of links.
    auto forms_link = [](DOM::Element const& element) {
        if (element.is_svg_a_element())
            return element.has_attribute(SVG::AttributeNames::href) || element.has_attribute_ns(Namespace::XLink, SVG::AttributeNames::href);
        return (element.is_html_anchor_element() || element.is_html_area_element()) && element.has_attribute(HTML::AttributeNames::href);
    };

    // https://svgwg.org/svg2-draft/linking.html#AElement
    // Links may not be nested; if an ‘a’ element is a descendent of another hyperlink element (whether in the SVG
    // namespace or another namespace), user agents must ignore its href attribute and treat it as inactive.
    auto is_valid_link = forms_link(*this);
    for (auto ancestor = parent_element(); is_valid_link && ancestor; ancestor = ancestor->parent_element()) {
        if (forms_link(*ancestor))
            is_valid_link = false;
    }
    return Base::is_focusable() || (is_valid_link && !is_in_never_rendered_subtree() && meets_focusable_area_rendering_requirements());
}

// https://svgwg.org/svg2-draft/linking.html#__svg__SVGAElement__target
GC::Ref<SVGAnimatedString> SVGAElement::target()
{
    if (!m_target)
        m_target = SVGAnimatedString::create(*this, DOM::QualifiedName { HTML::AttributeNames::target, OptionalNone {}, OptionalNone {} });
    return *m_target;
}

// https://svgwg.org/svg2-draft/linking.html#__svg__SVGAElement__relList
GC::Ref<DOM::DOMTokenList> SVGAElement::rel_list()
{
    // The relList IDL attribute reflects the ‘rel’ content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

CSS::ElementBoxKind SVGAElement::box_kind() const
{
    return CSS::ElementBoxKind::SvgGraphics;
}

void SVGAElement::activation_behavior(DOM::Event const& event)
{
    activate_the_hyperlink(event);
}

// AD-HOC: The URL of an SVG a element comes from its href attribute, or from the deprecated xlink:href attribute if
//         href is absent, so that the HyperlinkElementUtils members agree with the link the element follows.
// https://html.spec.whatwg.org/multipage/links.html#concept-hyperlink-url-set
void SVGAElement::set_the_url()
{
    auto should_invalidate_style = m_should_invalidate_style_after_url_change;
    m_should_invalidate_style_after_url_change = true;
    ScopeGuard invalidate_style_if_needed = [old_url = m_url, should_invalidate_style, this] {
        if (should_invalidate_style && m_url != old_url)
            CSS::Invalidation::invalidate_style_after_hyperlink_state_change(*this);
    };

    // 1. Set this element's url to null.
    m_url = {};

    // 2. If this element's href content attribute is absent, then return.
    auto href = attribute(AttributeNames::href);
    if (!href.has_value())
        href = get_attribute_ns(Namespace::XLink, AttributeNames::href);
    if (!href.has_value())
        return;

    // 3. Let url be the result of encoding-parsing a URL given this element's href content attribute's value, relative
    //    to this element's node document.
    auto url = document().encoding_parse_url(*href);

    // 4. If url is not failure, then set this element's url to url.
    if (url.has_value())
        m_url = url.release_value();
}

// AD-HOC: Updating the href of an SVG a element writes to the href attribute, unless only the deprecated xlink:href
//         attribute is present, in which case that attribute is written instead.
// https://html.spec.whatwg.org/multipage/links.html#update-href
void SVGAElement::update_href()
{
    auto serialized_url = utf16_string_from_url_ascii(m_url->serialize());
    if (!has_attribute(AttributeNames::href) && has_attribute_ns(Namespace::XLink, AttributeNames::href)) {
        set_attribute_value(AttributeNames::href, move(serialized_url), "xlink"_utf16_fly_string, Namespace::XLink);
        return;
    }
    set_attribute_value(AttributeNames::href, move(serialized_url));
}

}
