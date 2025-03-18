/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ShorthandStyleValue.h"
#include <LibGfx/Font/FontWeight.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

ShorthandStyleValue::ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>> values)
    : StyleValueWithDefaultOperators(Type::Shorthand)
    , m_properties { shorthand, move(sub_properties), move(values) }
{
    if (m_properties.sub_properties.size() != m_properties.values.size()) {
        dbgln("ShorthandStyleValue: sub_properties and values must be the same size! {} != {}", m_properties.sub_properties.size(), m_properties.values.size());
        VERIFY_NOT_REACHED();
    }
}

ShorthandStyleValue::~ShorthandStyleValue() = default;

ValueComparingRefPtr<CSSStyleValue const> ShorthandStyleValue::longhand(PropertyID longhand) const
{
    for (auto i = 0u; i < m_properties.sub_properties.size(); ++i) {
        if (m_properties.sub_properties[i] == longhand)
            return m_properties.values[i];
    }
    return nullptr;
}

String ShorthandStyleValue::to_string(SerializationMode mode) const
{
    // If all the longhands are the same CSS-wide keyword, just return that once.
    Optional<Keyword> built_in_keyword;
    bool all_same_keyword = true;
    for (auto& value : m_properties.values) {
        if (!value->is_css_wide_keyword()) {
            all_same_keyword = false;
            break;
        }
        auto keyword = value->to_keyword();
        if (!built_in_keyword.has_value()) {
            built_in_keyword = keyword;
            continue;
        }
        if (built_in_keyword != keyword) {
            all_same_keyword = false;
            break;
        }
    }
    if (all_same_keyword && built_in_keyword.has_value())
        return m_properties.values.first()->to_string(mode);

    // Then special cases
    switch (m_properties.shorthand_property) {
    case PropertyID::Background: {
        auto color = longhand(PropertyID::BackgroundColor);
        auto image = longhand(PropertyID::BackgroundImage);
        auto position = longhand(PropertyID::BackgroundPosition);
        auto position_x = position->as_shorthand().longhand(PropertyID::BackgroundPositionX);
        auto position_y = position->as_shorthand().longhand(PropertyID::BackgroundPositionY);
        auto size = longhand(PropertyID::BackgroundSize);
        auto repeat = longhand(PropertyID::BackgroundRepeat);
        auto attachment = longhand(PropertyID::BackgroundAttachment);
        auto origin = longhand(PropertyID::BackgroundOrigin);
        auto clip = longhand(PropertyID::BackgroundClip);

        auto get_layer_count = [](auto style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        auto layer_count = max(get_layer_count(image), max(get_layer_count(position_x), max(get_layer_count(position_y), max(get_layer_count(size), max(get_layer_count(repeat), max(get_layer_count(attachment), max(get_layer_count(origin), get_layer_count(clip))))))));

        if (layer_count == 1) {
            return MUST(String::formatted("{} {} {} {} {} {} {} {} {}", color->to_string(mode), image->to_string(mode), position_x->to_string(mode), position_y->to_string(mode), size->to_string(mode), repeat->to_string(mode), attachment->to_string(mode), origin->to_string(mode), clip->to_string(mode)));
        }

        auto get_layer_value_string = [mode](ValueComparingRefPtr<CSSStyleValue const> const& style_value, size_t index) {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true)->to_string(mode);
            return style_value->to_string(mode);
        };

        StringBuilder builder;
        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);
            if (i == layer_count - 1)
                builder.appendff("{} ", color->to_string(mode));
            builder.appendff("{} {} {} {} {} {} {} {}", get_layer_value_string(image, i), get_layer_value_string(position_x, i), get_layer_value_string(position_y, i), get_layer_value_string(size, i), get_layer_value_string(repeat, i), get_layer_value_string(attachment, i), get_layer_value_string(origin, i), get_layer_value_string(clip, i));
        }

        return MUST(builder.to_string());
    }
    case Web::CSS::PropertyID::BackgroundPosition: {
        auto x_edges = longhand(PropertyID::BackgroundPositionX);
        auto y_edges = longhand(PropertyID::BackgroundPositionY);

        auto get_layer_count = [](auto style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        // FIXME: The spec is unclear about how differing layer counts should be handled
        auto layer_count = max(get_layer_count(x_edges), get_layer_count(y_edges));

        if (layer_count == 1) {
            return MUST(String::formatted("{} {}", x_edges->to_string(mode), y_edges->to_string(mode)));
        }

        auto get_layer_value_string = [mode](ValueComparingRefPtr<CSSStyleValue const> const& style_value, size_t index) {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true)->to_string(mode);
            return style_value->to_string(mode);
        };

        StringBuilder builder;
        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);

            builder.appendff("{} {}", get_layer_value_string(x_edges, i), get_layer_value_string(y_edges, i));
        }

        return MUST(builder.to_string());
    }
    case PropertyID::BorderRadius: {
        auto top_left = longhand(PropertyID::BorderTopLeftRadius);
        auto top_right = longhand(PropertyID::BorderTopRightRadius);
        auto bottom_right = longhand(PropertyID::BorderBottomRightRadius);
        auto bottom_left = longhand(PropertyID::BorderBottomLeftRadius);

        auto horizontal_radius = [&](auto& style_value) -> String {
            if (style_value->is_border_radius())
                return style_value->as_border_radius().horizontal_radius().to_string();
            return style_value->to_string(mode);
        };

        auto top_left_horizontal_string = horizontal_radius(top_left);
        auto top_right_horizontal_string = horizontal_radius(top_right);
        auto bottom_right_horizontal_string = horizontal_radius(bottom_right);
        auto bottom_left_horizontal_string = horizontal_radius(bottom_left);

        auto vertical_radius = [&](auto& style_value) -> String {
            if (style_value->is_border_radius())
                return style_value->as_border_radius().vertical_radius().to_string();
            return style_value->to_string(mode);
        };

        auto top_left_vertical_string = vertical_radius(top_left);
        auto top_right_vertical_string = vertical_radius(top_right);
        auto bottom_right_vertical_string = vertical_radius(bottom_right);
        auto bottom_left_vertical_string = vertical_radius(bottom_left);

        auto serialize_radius = [](auto top_left, auto const& top_right, auto const& bottom_right, auto const& bottom_left) -> String {
            if (first_is_equal_to_all_of(top_left, top_right, bottom_right, bottom_left))
                return top_left;
            if (top_left == bottom_right && top_right == bottom_left)
                return MUST(String::formatted("{} {}", top_left, top_right));
            if (top_right == bottom_left)
                return MUST(String::formatted("{} {} {}", top_left, top_right, bottom_right));

            return MUST(String::formatted("{} {} {} {}", top_left, top_right, bottom_right, bottom_left));
        };

        auto first_radius_serialization = serialize_radius(move(top_left_horizontal_string), top_right_horizontal_string, bottom_right_horizontal_string, bottom_left_horizontal_string);
        auto second_radius_serialization = serialize_radius(move(top_left_vertical_string), top_right_vertical_string, bottom_right_vertical_string, bottom_left_vertical_string);
        if (first_radius_serialization == second_radius_serialization)
            return first_radius_serialization;

        return MUST(String::formatted("{} / {}", first_radius_serialization, second_radius_serialization));
    }
    case PropertyID::Columns: {
        auto column_width = longhand(PropertyID::ColumnWidth)->to_string(mode);
        auto column_count = longhand(PropertyID::ColumnCount)->to_string(mode);

        if (column_width == column_count)
            return column_width;
        if (column_width.equals_ignoring_ascii_case("auto"sv))
            return column_count;
        if (column_count.equals_ignoring_ascii_case("auto"sv))
            return column_width;

        return MUST(String::formatted("{} {}", column_width, column_count));
    }
    case PropertyID::Flex:
        return MUST(String::formatted("{} {} {}", longhand(PropertyID::FlexGrow)->to_string(mode), longhand(PropertyID::FlexShrink)->to_string(mode), longhand(PropertyID::FlexBasis)->to_string(mode)));
    case PropertyID::FlexFlow:
        return MUST(String::formatted("{} {}", longhand(PropertyID::FlexDirection)->to_string(mode), longhand(PropertyID::FlexWrap)->to_string(mode)));
    case PropertyID::Font: {
        auto font_style = longhand(PropertyID::FontStyle);
        auto font_variant = longhand(PropertyID::FontVariant);
        auto font_weight = longhand(PropertyID::FontWeight);
        auto font_width = longhand(PropertyID::FontWidth);
        auto font_size = longhand(PropertyID::FontSize);
        auto line_height = longhand(PropertyID::LineHeight);
        auto font_family = longhand(PropertyID::FontFamily);

        // Some longhands prevent serialization if they are not allowed in the shorthand.
        // <font-variant-css2> = normal | small-caps
        auto font_variant_string = font_variant->to_string(mode);
        if (!first_is_one_of(font_variant_string, "normal"sv, "small-caps"sv) && !CSS::is_css_wide_keyword(font_variant_string)) {
            return {};
        }
        // <font-width-css3> = normal | ultra-condensed | extra-condensed | condensed | semi-condensed | semi-expanded | expanded | extra-expanded | ultra-expanded
        switch (font_width->to_keyword()) {
        case Keyword::Initial:
        case Keyword::Normal:
        case Keyword::UltraCondensed:
        case Keyword::ExtraCondensed:
        case Keyword::Condensed:
        case Keyword::SemiCondensed:
        case Keyword::SemiExpanded:
        case Keyword::Expanded:
        case Keyword::ExtraExpanded:
        case Keyword::UltraExpanded:
            break;
        default:
            if (!font_width->is_css_wide_keyword())
                return {};
        }

        StringBuilder builder;
        auto append = [&](auto const& string) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.append(string);
        };
        if (font_style->to_keyword() != Keyword::Normal && font_style->to_keyword() != Keyword::Initial)
            append(font_style->to_string(mode));
        if (font_variant_string != "normal"sv && font_variant_string != "initial"sv)
            append(font_variant_string);
        if (font_weight->to_font_weight() != Gfx::FontWeight::Regular && font_weight->to_keyword() != Keyword::Initial)
            append(font_weight->to_string(mode));
        if (font_width->to_keyword() != Keyword::Normal && font_width->to_keyword() != Keyword::Initial)
            append(font_width->to_string(mode));
        append(font_size->to_string(mode));
        if (line_height->to_keyword() != Keyword::Normal && line_height->to_keyword() != Keyword::Initial)
            append(MUST(String::formatted("/ {}", line_height->to_string(mode))));
        append(font_family->to_string(mode));

        return builder.to_string_without_validation();
    }
    case PropertyID::FontVariant: {
        auto ligatures = longhand(PropertyID::FontVariantLigatures);
        auto caps = longhand(PropertyID::FontVariantCaps);
        auto alternates = longhand(PropertyID::FontVariantAlternates);
        auto numeric = longhand(PropertyID::FontVariantNumeric);
        auto east_asian = longhand(PropertyID::FontVariantEastAsian);
        auto position = longhand(PropertyID::FontVariantPosition);
        auto emoji = longhand(PropertyID::FontVariantEmoji);

        Vector<String> values;
        if (ligatures->to_keyword() != Keyword::Normal)
            values.append(ligatures->to_string(mode));
        if (caps->to_keyword() != Keyword::Normal)
            values.append(caps->to_string(mode));
        if (alternates->to_keyword() != Keyword::Normal)
            values.append(alternates->to_string(mode));
        if (numeric->to_keyword() != Keyword::Normal)
            values.append(numeric->to_string(mode));
        if (east_asian->to_keyword() != Keyword::Normal)
            values.append(east_asian->to_string(mode));
        if (position->to_keyword() != Keyword::Normal)
            values.append(position->to_string(mode));
        if (emoji->to_keyword() != Keyword::Normal)
            values.append(emoji->to_string(mode));

        if (values.is_empty())
            return "normal"_string;
        return MUST(String::join(' ', values));
    }
    case PropertyID::GridArea: {
        auto& row_start = longhand(PropertyID::GridRowStart)->as_grid_track_placement();
        auto& column_start = longhand(PropertyID::GridColumnStart)->as_grid_track_placement();
        auto& row_end = longhand(PropertyID::GridRowEnd)->as_grid_track_placement();
        auto& column_end = longhand(PropertyID::GridColumnEnd)->as_grid_track_placement();
        StringBuilder builder;
        if (!row_start.grid_track_placement().is_auto())
            builder.appendff("{}", row_start.grid_track_placement().to_string());
        if (!column_start.grid_track_placement().is_auto())
            builder.appendff(" / {}", column_start.grid_track_placement().to_string());
        if (!row_end.grid_track_placement().is_auto())
            builder.appendff(" / {}", row_end.grid_track_placement().to_string());
        if (!column_end.grid_track_placement().is_auto())
            builder.appendff(" / {}", column_end.grid_track_placement().to_string());
        return MUST(builder.to_string());
    }
        // FIXME: Serialize Grid differently once we support it better!
    case PropertyID::Grid:
    case PropertyID::GridTemplate: {
        auto& areas = longhand(PropertyID::GridTemplateAreas)->as_grid_template_area();
        auto& rows = longhand(PropertyID::GridTemplateRows)->as_grid_track_size_list();
        auto& columns = longhand(PropertyID::GridTemplateColumns)->as_grid_track_size_list();

        auto construct_rows_string = [&]() {
            StringBuilder builder;
            size_t idx = 0;
            for (auto const& row : rows.grid_track_size_list().track_list()) {
                if (areas.grid_template_area().size() > idx) {
                    builder.append("\""sv);
                    for (size_t y = 0; y < areas.grid_template_area()[idx].size(); ++y) {
                        builder.append(areas.grid_template_area()[idx][y]);
                        if (y != areas.grid_template_area()[idx].size() - 1)
                            builder.append(" "sv);
                    }
                    builder.append("\" "sv);
                }
                builder.append(row.to_string());
                if (idx < rows.grid_track_size_list().track_list().size() - 1)
                    builder.append(' ');
                idx++;
            }
            return MUST(builder.to_string());
        };

        if (columns.grid_track_size_list().track_list().size() == 0)
            return MUST(String::formatted("{}", construct_rows_string()));
        return MUST(String::formatted("{} / {}", construct_rows_string(), columns.grid_track_size_list().to_string()));
    }
    case PropertyID::GridColumn: {
        auto start = longhand(PropertyID::GridColumnStart);
        auto end = longhand(PropertyID::GridColumnEnd);
        if (end->as_grid_track_placement().grid_track_placement().is_auto() || start == end)
            return start->to_string(mode);
        return MUST(String::formatted("{} / {}", start->to_string(mode), end->to_string(mode)));
    }
    case PropertyID::GridRow: {
        auto start = longhand(PropertyID::GridRowStart);
        auto end = longhand(PropertyID::GridRowEnd);
        if (end->as_grid_track_placement().grid_track_placement().is_auto() || start == end)
            return start->to_string(mode);
        return MUST(String::formatted("{} / {}", start->to_string(mode), end->to_string(mode)));
    }
    case PropertyID::ListStyle:
        return MUST(String::formatted("{} {} {}", longhand(PropertyID::ListStylePosition)->to_string(mode), longhand(PropertyID::ListStyleImage)->to_string(mode), longhand(PropertyID::ListStyleType)->to_string(mode)));
    case PropertyID::Overflow: {
        auto overflow_x = longhand(PropertyID::OverflowX);
        auto overflow_y = longhand(PropertyID::OverflowY);
        if (overflow_x == overflow_y)
            return overflow_x->to_string(mode);

        return MUST(String::formatted("{} {}", overflow_x->to_string(mode), overflow_y->to_string(mode)));
    }
    case PropertyID::PlaceContent: {
        auto align_content = longhand(PropertyID::AlignContent)->to_string(mode);
        auto justify_content = longhand(PropertyID::JustifyContent)->to_string(mode);
        if (align_content == justify_content)
            return align_content;
        return MUST(String::formatted("{} {}", align_content, justify_content));
    }
    case PropertyID::PlaceItems: {
        auto align_items = longhand(PropertyID::AlignItems)->to_string(mode);
        auto justify_items = longhand(PropertyID::JustifyItems)->to_string(mode);
        if (align_items == justify_items)
            return align_items;
        return MUST(String::formatted("{} {}", align_items, justify_items));
    }
    case PropertyID::PlaceSelf: {
        auto align_self = longhand(PropertyID::AlignSelf)->to_string(mode);
        auto justify_self = longhand(PropertyID::JustifySelf)->to_string(mode);
        if (align_self == justify_self)
            return align_self;
        return MUST(String::formatted("{} {}", align_self, justify_self));
    }
    case PropertyID::TextDecoration: {
        // The rule here seems to be, only print what's different from the default value,
        // but if they're all default, print the line.
        StringBuilder builder;
        auto append_if_non_default = [&](PropertyID property_id) {
            auto value = longhand(property_id);
            if (!value->equals(property_initial_value(property_id))) {
                if (!builder.is_empty())
                    builder.append(' ');
                builder.append(value->to_string(mode));
            }
        };

        append_if_non_default(PropertyID::TextDecorationLine);
        append_if_non_default(PropertyID::TextDecorationThickness);
        append_if_non_default(PropertyID::TextDecorationStyle);
        append_if_non_default(PropertyID::TextDecorationColor);

        if (builder.is_empty())
            return longhand(PropertyID::TextDecorationLine)->to_string(mode);

        return builder.to_string_without_validation();
    }
    default:
        StringBuilder builder;
        auto first = true;
        for (auto& value : m_properties.values) {
            if (first)
                first = false;
            else
                builder.append(' ');
            builder.append(value->to_string(mode));
        }
        return MUST(builder.to_string());
    }
}

}
