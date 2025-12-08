/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Bindings/HTMLAreaElementPrototype.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAreaElement);

HTMLAreaElement::HTMLAreaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLAreaElement::~HTMLAreaElement() = default;

void HTMLAreaElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAreaElement);
    Base::initialize(realm);
}

void HTMLAreaElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rel_list);
}

void HTMLAreaElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == HTML::AttributeNames::href) {
        set_the_url();
    } else if (name == HTML::AttributeNames::rel) {
        if (m_rel_list)
            m_rel_list->associated_attribute_changed(value.value_or(String {}));
    }
}

// https://html.spec.whatwg.org/multipage/image-maps.html#dom-area-rellist
GC::Ref<DOM::DOMTokenList> HTMLAreaElement::rel_list()
{
    // The IDL attribute relList must reflect the rel content attribute.
    if (!m_rel_list)
        m_rel_list = DOM::DOMTokenList::create(*this, HTML::AttributeNames::rel);
    return *m_rel_list;
}

Optional<String> HTMLAreaElement::hyperlink_element_utils_href() const
{
    return attribute(HTML::AttributeNames::href);
}

void HTMLAreaElement::set_hyperlink_element_utils_href(String href)
{
    set_attribute_value(HTML::AttributeNames::href, move(href));
}

Optional<String> HTMLAreaElement::hyperlink_element_utils_referrerpolicy() const
{
    return attribute(HTML::AttributeNames::referrerpolicy);
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLAreaElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

Optional<ARIA::Role> HTMLAreaElement::default_role() const
{
    // https://www.w3.org/TR/html-aria/#el-area-no-href
    if (!href().is_empty())
        return ARIA::Role::link;
    // https://www.w3.org/TR/html-aria/#el-area
    return ARIA::Role::generic;
}

// https://html.spec.whatwg.org/multipage/links.html#links-created-by-a-and-area-elements
void HTMLAreaElement::activate(Web::DOM::Event const& event)
{
    // See implementation of activation_behavior of an anchor tag for reference.
    // Step (3) has been omitted for a lack of observed relevance. Following from the EventHandler only HTMLImageElements with usemap will activate an area tag.

    // The activation behavior of an a or area element element given an event event is:

    // 1. If element has no href attribute, then return.
    if (href().is_empty())
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

    // 3. Let userInvolvement be event's user navigation involvement.
    auto user_involvement = user_navigation_involvement(event);

    // 4. If the user has expressed a preference to download the hyperlink, then set userInvolvement to "browser UI".
    // NOTE: That is, if the user has expressed a specific preference for downloading, this no longer counts as merely "activation".
    if (has_download_preference())
        user_involvement = UserNavigationInvolvement::BrowserUI;

    // FIXME: 5. If element has a download attribute, or if the user has expressed a preference to download the
    //     hyperlink, then download the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and
    //     userInvolvement set to userInvolvement.

    // 6. Otherwise, follow the hyperlink created by element with hyperlinkSuffix set to hyperlinkSuffix and userInvolvement set to userInvolvement.
    follow_the_hyperlink(hyperlink_suffix, user_involvement);
}

bool HTMLAreaElement::has_download_preference() const
{
    return has_attribute(HTML::AttributeNames::download);
}

bool HTMLAreaElement::check_if_contains_point(Gfx::IntPoint point) const
{
    auto shape = attribute(HTML::AttributeNames::shape);

    auto coords = attribute(HTML::AttributeNames::coords);
    if (!coords.has_value() && shape.has_value())
        return false;

    // Parse coordinates for the area.
    // https://html.spec.whatwg.org/multipage/common-microsyntaxes.html#rules-for-parsing-a-list-of-floating-point-numbers
    Vector<float> coords_list;
    if (coords.has_value()) {
        String coords_string = coords.release_value();
        auto parts_or_error = coords_string.split(',');
        if (parts_or_error.is_error())
            return false;

        auto const& parts = parts_or_error.value();
        for (auto const& part : parts) {
            coords_list.append(part.to_number<float>().value());
        }
    }

    // https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
    // If the number of items in the coords list is less than the minimum number given for the area element's current state, as per the following table, then the shape is empty; return.
    // For excess coordinates, see shape-by-shape behavior.
    if (shape == "rect"sv) {
        if (coords_list.size() < 4)
            return false;

        auto left = coords_list[0];
        auto top = coords_list[1];
        auto right = coords_list[2];
        auto bottom = coords_list[3];

        return point.x() >= left && point.x() <= right 
            && point.y() >= top && point.y() <= bottom;
    }
    if (shape == "circle"sv) {
        if (coords_list.size() < 3)
            return false;

        auto center_x_position_from_left = coords_list[0];
        auto center_y_position_from_top = coords_list[1];
        auto radius = coords_list[2];

        float difference_in_x = point.x() - center_x_position_from_left;
        float difference_in_y = point.y() - center_y_position_from_top;
        return (difference_in_x * difference_in_x + difference_in_y * difference_in_y) <= (radius * radius);
    }
    if (shape == "poly"sv) {
        if (coords_list.size() < 6)
            return false;

        Vector<Gfx::FloatPoint> polygon_coordinate_representation;
        for (size_t i = 0; i < coords_list.size() - (coords_list.size() % 2); i += 2)
            polygon_coordinate_representation.append({ coords_list[i], coords_list[i + 1] });

        bool inside = false;
        for (size_t i = 0, j = polygon_coordinate_representation.size() - 1; i < polygon_coordinate_representation.size(); j = i++) {
            auto& coordinate_i = polygon_coordinate_representation[i];
            auto& coordinate_j = polygon_coordinate_representation[j];

            bool intersect = ((coordinate_i.y() > point.y()) != (coordinate_j.y() > point.y()))
                && (point.x() < (coordinate_j.x() - coordinate_i.x()) * (point.y() - coordinate_i.y()) / (coordinate_j.y() - coordinate_i.y()) + coordinate_i.x());

            if (intersect)
                inside = !inside;
        }
        return inside;
    }

    // Default area is the image, thus always contains the clicked point.
    return true;
}

}
