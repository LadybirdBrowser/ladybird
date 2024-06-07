/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLAnchorElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLAnchorElement);

HTMLAnchorElement::HTMLAnchorElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAnchorElement::~HTMLAnchorElement() = default;

void HTMLAnchorElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAnchorElement);
}

void HTMLAnchorElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAnchorElement::attribute_changed(FlyString const& name, Optional<String> const& value)
{
    HTMLElement::attribute_changed(name, value);
    if (name == HTML::AttributeNames::href) {
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

Optional<String> HTMLAnchorElement::hyperlink_element_utils_href() const
{
    return attribute(HTML::AttributeNames::href);
}

WebIDL::ExceptionOr<void> HTMLAnchorElement::set_hyperlink_element_utils_href(String href)
{
    return set_attribute(HTML::AttributeNames::href, move(href));
}

Optional<String> HTMLAnchorElement::hyperlink_element_utils_referrerpolicy() const
{
    return attribute(HTML::AttributeNames::referrerpolicy);
}

bool HTMLAnchorElement::has_activation_behavior() const
{
    return true;
}

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
void HTMLAnchorElement::activation_behavior(Web::DOM::Event const& event)
{
    // The activation behavior of an a or area element element given an event event is:

    // 1. If element has no href attribute, then return.
    if (href().is_empty())
        return;

    // 2. Let hyperlinkSuffix be null.
    Optional<String> hyperlink_suffix {};

    // 3. If element is an a element, and event's target is an img with an ismap attribute specified, then:
    if (event.target() && is<HTMLImageElement>(*event.target()) && static_cast<HTMLImageElement const&>(*event.target()).has_attribute(AttributeNames::ismap)) {
        // 1. Let x and y be 0.
        CSSPixels x { 0 };
        CSSPixels y { 0 };

        // 2. If event's isTrusted attribute is initialized to true, then set x to the distance in CSS pixels from the left edge of the image
        //    to the location of the click, and set y to the distance in CSS pixels from the top edge of the image to the location of the click.
        if (event.is_trusted() && is<UIEvents::MouseEvent>(event)) {
            auto const& mouse_event = static_cast<UIEvents::MouseEvent const&>(event);
            x = CSSPixels { mouse_event.offset_x() };
            y = CSSPixels { mouse_event.offset_y() };
        }

        // 3. If x is negative, set x to 0.
        x = max(x, 0);

        // 4. If y is negative, set y to 0.
        y = max(y, 0);

        // 5. Set hyperlinkSuffix to the concatenation of U+003F (?), the value of x expressed as a base-ten integer using ASCII digits,
        //    U+002C (,), and the value of y expressed as a base-ten integer using ASCII digits.
        hyperlink_suffix = MUST(String::formatted("?{},{}", x.to_int(), y.to_int()));
    }

    // 4. Let userInvolvement be event's user navigation involvement.
    auto user_involvement = user_navigation_involvement(event);

    // FIXME: 5. If the user has expressed a preference to download the hyperlink, then set userInvolvement to "browser UI".
    // NOTE: That is, if the user has expressed a specific preference for downloading, this no longer counts as merely "activation".

    // FIXME: 6. If element has a download attribute, or if the user has expressed a preference to download the
    //     hyperlink, then download the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and
    //     userInvolvement set to userInvolvement.

    // 7. Otherwise, follow the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and userInvolvement set to userInvolvement.
    follow_the_hyperlink(hyperlink_suffix, user_involvement);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAnchorElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

Optional<ARIA::Role> HTMLAnchorElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-a-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-a
    return ARIA::Role::generic;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-rellist
JS::NonnullGCPtr<DOM::DOMTokenList> HTMLAnchorElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
String HTMLAnchorElement::text() const
{
    // The text attribute's getter must return this element's descendant text content.
    return descendant_text_content();
}

// https://html.spec.whatwg.org/multipage/text-level-semantics.html#dom-a-text
void HTMLAnchorElement::set_text(String const& text)
{
    // The text attribute's setter must string replace all with the given value within this element.
    string_replace_all(text);
}

}
