/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGAElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAElement.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAElement);

SVGAElement::SVGAElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGraphicsElement(document, move(qualified_name))
{
}

SVGAElement::~SVGAElement() = default;

void SVGAElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGAElement);
    Base::initialize(realm);
}

void SVGAElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
    visitor.visit(m_rel_list);
    visitor.visit(m_target);
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

// https://svgwg.org/svg2-draft/linking.html#__svg__SVGAElement__target
GC::Ref<SVGAnimatedString> SVGAElement::target()
{
    if (!m_target)
        m_target = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { HTML::AttributeNames::target, OptionalNone {}, OptionalNone {} });
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

GC::Ptr<Layout::Node> SVGAElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGGraphicsBox>(document(), *this, move(style));
}

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
void SVGAElement::activation_behavior(DOM::Event const& event)
{
    // The activation behavior of an a or area element element given an event event is:

    // 1. If element has no href attribute, then return.
    if (href()->base_val().is_empty())
        return;

    // AD-HOC: Do not activate the element for clicks with the ctrl/cmd modifier present. This lets
    //         the browser process open the link in a new tab.
    if (is<UIEvents::MouseEvent>(event)) {
        auto const& mouse_event = static_cast<UIEvents::MouseEvent const&>(event);
        if (mouse_event.platform_ctrl_key())
            return;
    }

    // 2. Let hyperlinkSuffix be null.
    Optional<String> hyperlink_suffix {};

    // FIXME: 3. If element is an a element, and event's target is an img with an ismap attribute specified, then:

    // 4. Let userInvolvement be event's user navigation involvement.
    auto user_involvement = HTML::user_navigation_involvement(event);

    // FIXME: 5. If the user has expressed a preference to download the hyperlink, then set userInvolvement to "browser UI".

    // FIXME: 6. If element has a download attribute, or if the user has expressed a preference to download the
    //     hyperlink, then download the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and
    //     userInvolvement set to userInvolvement.

    // 7. Otherwise, follow the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and userInvolvement set to userInvolvement.
    follow_the_hyperlink(hyperlink_suffix, user_involvement);
}

}
