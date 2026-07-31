/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Painting/MarkerPaintable.h>

namespace Web::Layout {

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, CSS::ListStyleType style_type, CSS::ListStylePosition style_position, NonnullRefPtr<CSS::ComputedValues const> style)
    : BlockContainer(document, nullptr, style)
    , m_list_style_type(style_type)
    , m_list_style_position(style_position)
{
}

bool ListItemMarkerBox::has_symbolic_counter_style() const
{
    auto const* counter_style = m_list_style_type.get_pointer<RefPtr<CSS::CounterStyle const>>();
    return counter_style && counter_style_is_rendered_with_custom_image(*counter_style);
}

bool ListItemMarkerBox::is_symbolic() const
{
    if (computed_values().computed_content().type != CSS::ComputedContentData::Type::Normal)
        return false;
    return list_style_image() || has_symbolic_counter_style();
}

ListItemMarkerBox::~ListItemMarkerBox() = default;

bool ListItemMarkerBox::counter_style_is_rendered_with_custom_image(RefPtr<CSS::CounterStyle const> const& counter_style)
{
    // https://drafts.csswg.org/css-counter-styles-3/#simple-symbolic
    // When used in list-style-type, a UA may instead render these styles using a UA-generated image or a UA-chosen font
    // instead of rendering the specified character in the element’s own font. If using an image, it must look similar
    // to the character, and must be sized to attractively fill a 1em by 1em square.

    if (!counter_style)
        return false;

    auto const& counter_style_name = counter_style->name();

    return first_is_one_of(counter_style_name, "square"_utf16_fly_string, "circle"_utf16_fly_string, "disc"_utf16_fly_string, "disclosure-closed"_utf16_fly_string, "disclosure-open"_utf16_fly_string);
}

RefPtr<Painting::Paintable> ListItemMarkerBox::create_paintable() const
{
    if (!is_symbolic())
        return BlockContainer::create_paintable();
    return Painting::MarkerPaintable::create(*this);
}

CSSPixels ListItemMarkerBox::relative_size() const
{
    VERIFY(has_symbolic_counter_style());

    // https://drafts.csswg.org/css-counter-styles-3/#simple-symbolic
    // NB: The spec allows us to render some predefined symbol counter styles using a UA-generated image instead of
    //     text, it instructs us to size these in order to attractively fit within a 1em x 1em square. We mimic Firefox
    //     and generally use a size of 0.35em, except for disclosure open/closed styles which use a size of 0.5em.
    static constexpr float marker_image_size_factor = 0.35f;
    static constexpr float disclosure_marker_image_size_factor = 0.5f;

    auto const& counter_style_name = m_list_style_type.get<RefPtr<CSS::CounterStyle const>>()->name();
    bool is_disclosure = first_is_one_of(counter_style_name, "disclosure-closed"_utf16_fly_string, "disclosure-open"_utf16_fly_string);
    auto size_factor = is_disclosure ? disclosure_marker_image_size_factor : marker_image_size_factor;
    return CSSPixels::nearest_value_for(ceilf(first_available_font().pixel_size() * size_factor));
}

}
