/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Painting/MarkerPaintable.h>

namespace Web::Layout {

ListItemMarkerBox::ListItemMarkerBox(DOM::Document& document, CSS::ListStyleType style_type, CSS::ListStylePosition style_position, GC::Ref<DOM::Element> list_item_element, NonnullRefPtr<CSS::ComputedValues const> style)
    : Box(document, nullptr, style)
    , m_list_style_type(style_type)
    , m_list_style_position(style_position)
    , m_list_item_element(list_item_element)
{
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

Optional<Utf16String> ListItemMarkerBox::text() const
{
    VERIFY(m_list_item_element);

    // https://drafts.csswg.org/css-lists-3/#text-markers
    auto index = m_list_item_element->ordinal_value();

    auto generate_from_counter_style = [&](RefPtr<CSS::CounterStyle const> const& counter_style) -> Optional<Utf16String> {
        if (counter_style_is_rendered_with_custom_image(counter_style))
            return {};

        auto counter_representation = CSS::generate_a_counter_representation(counter_style, DOM::AbstractElement { *m_list_item_element }.style_scope(), index);

        if (!counter_style)
            return Utf16String::formatted("{}. ", counter_representation);

        return Utf16String::formatted("{}{}{}", counter_style->prefix(), counter_representation, counter_style->suffix());
    };

    return m_list_style_type.visit(
        [](Empty const&) -> Optional<Utf16String> {
            // none
            // The element has no marker string.
            return {};
        },
        [&](RefPtr<CSS::CounterStyle const> const& counter_style) -> Optional<Utf16String> {
            // <counter-style>
            // Specifies the element’s marker string as the value of the list-item counter represented using the
            // specified <counter-style>. Specifically, the marker string is the result of generating a counter
            // representation of the list-item counter value using the specified <counter-style>, prefixed by the prefix
            // of the <counter-style>, and followed by the suffix of the <counter-style>. If the specified
            // <counter-style> does not exist, decimal is assumed.
            return generate_from_counter_style(counter_style);
        },
        [](Utf16String const& string) -> Optional<Utf16String> {
            // <string>
            // The element’s marker string is the specified <string>.
            return string;
        },
        [&](Utf16FlyString const&) -> Optional<Utf16String> {
            // NB: An unresolved <counter-style> falls back to decimal.
            auto counter_representation = CSS::generate_a_counter_representation(nullptr, DOM::AbstractElement { *m_list_item_element }.style_scope(), index);
            return Utf16String::formatted("{}. ", counter_representation);
        },
        [&](CSS::ListStyleSymbols const& symbols) -> Optional<Utf16String> {
            return generate_from_counter_style(symbols.counter_style);
        });
}

RefPtr<Painting::Paintable> ListItemMarkerBox::create_paintable() const
{
    return Painting::MarkerPaintable::create(*this);
}

CSSPixels ListItemMarkerBox::relative_size() const
{
    VERIFY(!m_list_style_type.has<Empty>());

    auto font_size = first_available_font().pixel_size();
    auto marker_text = text();
    if (marker_text.has_value())
        return CSSPixels::nearest_value_for(font_size);

    // https://drafts.csswg.org/css-counter-styles-3/#simple-symbolic
    // NB: The spec allows us to render some predefined symbol counter styles using a UA-generated image instead of
    //     text, it instructs us to size these in order to attractively fit within a 1em x 1em square. We mimic Firefox
    //     and generally use a size of 0.35em, except for disclosure open/closed styles which use a size of 0.5em.
    static constexpr float marker_image_size_factor = 0.35f;
    static constexpr float disclosure_marker_image_size_factor = 0.5f;

    auto const& counter_style = m_list_style_type.get<RefPtr<CSS::CounterStyle const>>();

    VERIFY(counter_style);

    if (counter_style->name() == "square"_utf16_fly_string || counter_style->name() == "circle"_utf16_fly_string || counter_style->name() == "disc"_utf16_fly_string)
        return CSSPixels::nearest_value_for(ceilf(font_size * marker_image_size_factor));

    if (counter_style->name() == "disclosure-closed"_utf16_fly_string || counter_style->name() == "disclosure-open"_utf16_fly_string)
        return CSSPixels::nearest_value_for(ceilf(font_size * disclosure_marker_image_size_factor));

    VERIFY_NOT_REACHED();
}

}
