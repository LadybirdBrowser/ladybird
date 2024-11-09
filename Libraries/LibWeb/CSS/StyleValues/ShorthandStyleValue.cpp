/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ShorthandStyleValue.h"
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
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

String ShorthandStyleValue::to_string() const
{
    // Special-cases first
    switch (m_properties.shorthand_property) {
    case PropertyID::Background: {
        auto color = longhand(PropertyID::BackgroundColor);
        auto image = longhand(PropertyID::BackgroundImage);
        auto position = longhand(PropertyID::BackgroundPosition);
        auto size = longhand(PropertyID::BackgroundSize);
        auto repeat = longhand(PropertyID::BackgroundRepeat);
        auto attachment = longhand(PropertyID::BackgroundAttachment);
        auto origin = longhand(PropertyID::BackgroundOrigin);
        auto clip = longhand(PropertyID::BackgroundClip);

        auto get_layer_count = [](auto style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        auto layer_count = max(get_layer_count(image), max(get_layer_count(position), max(get_layer_count(size), max(get_layer_count(repeat), max(get_layer_count(attachment), max(get_layer_count(origin), get_layer_count(clip)))))));

        if (layer_count == 1) {
            return MUST(String::formatted("{} {} {} {} {} {} {} {}", color->to_string(), image->to_string(), position->to_string(), size->to_string(), repeat->to_string(), attachment->to_string(), origin->to_string(), clip->to_string()));
        }

        auto get_layer_value_string = [](ValueComparingRefPtr<CSSStyleValue const> const& style_value, size_t index) {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true)->to_string();
            return style_value->to_string();
        };

        StringBuilder builder;
        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);
            if (i == layer_count - 1)
                builder.appendff("{} ", color->to_string());
            builder.appendff("{} {} {} {} {} {} {}", get_layer_value_string(image, i), get_layer_value_string(position, i), get_layer_value_string(size, i), get_layer_value_string(repeat, i), get_layer_value_string(attachment, i), get_layer_value_string(origin, i), get_layer_value_string(clip, i));
        }

        return MUST(builder.to_string());
    }
    case PropertyID::BorderRadius: {
        auto& top_left = longhand(PropertyID::BorderTopLeftRadius)->as_border_radius();
        auto& top_right = longhand(PropertyID::BorderTopRightRadius)->as_border_radius();
        auto& bottom_right = longhand(PropertyID::BorderBottomRightRadius)->as_border_radius();
        auto& bottom_left = longhand(PropertyID::BorderBottomLeftRadius)->as_border_radius();

        return MUST(String::formatted("{} {} {} {} / {} {} {} {}",
            top_left.horizontal_radius().to_string(),
            top_right.horizontal_radius().to_string(),
            bottom_right.horizontal_radius().to_string(),
            bottom_left.horizontal_radius().to_string(),
            top_left.vertical_radius().to_string(),
            top_right.vertical_radius().to_string(),
            bottom_right.vertical_radius().to_string(),
            bottom_left.vertical_radius().to_string()));
    }
    case PropertyID::Columns: {
        auto column_width = longhand(PropertyID::ColumnWidth)->to_string();
        auto column_count = longhand(PropertyID::ColumnCount)->to_string();

        if (column_width == column_count)
            return column_width;
        if (column_width.equals_ignoring_ascii_case("auto"sv))
            return column_count;
        if (column_count.equals_ignoring_ascii_case("auto"sv))
            return column_width;

        return MUST(String::formatted("{} {}", column_width, column_count));
    }
    case PropertyID::Flex:
        return MUST(String::formatted("{} {} {}", longhand(PropertyID::FlexGrow)->to_string(), longhand(PropertyID::FlexShrink)->to_string(), longhand(PropertyID::FlexBasis)->to_string()));
    case PropertyID::FlexFlow:
        return MUST(String::formatted("{} {}", longhand(PropertyID::FlexDirection)->to_string(), longhand(PropertyID::FlexWrap)->to_string()));
    case PropertyID::Font:
        return MUST(String::formatted("{} {} {} {} {} / {} {}",
            longhand(PropertyID::FontStyle)->to_string(),
            longhand(PropertyID::FontVariant)->to_string(),
            longhand(PropertyID::FontWeight)->to_string(),
            longhand(PropertyID::FontWidth)->to_string(),
            longhand(PropertyID::FontSize)->to_string(),
            longhand(PropertyID::LineHeight)->to_string(),
            longhand(PropertyID::FontFamily)->to_string()));
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
        if (end->as_grid_track_placement().grid_track_placement().is_auto())
            return start->to_string();
        return MUST(String::formatted("{} / {}", start->to_string(), end->to_string()));
    }
    case PropertyID::GridRow: {
        auto start = longhand(PropertyID::GridRowStart);
        auto end = longhand(PropertyID::GridRowEnd);
        if (end->as_grid_track_placement().grid_track_placement().is_auto())
            return start->to_string();
        return MUST(String::formatted("{} / {}", start->to_string(), end->to_string()));
    }
    case PropertyID::ListStyle:
        return MUST(String::formatted("{} {} {}", longhand(PropertyID::ListStylePosition)->to_string(), longhand(PropertyID::ListStyleImage)->to_string(), longhand(PropertyID::ListStyleType)->to_string()));
    case PropertyID::Overflow:
        return MUST(String::formatted("{} {}", longhand(PropertyID::OverflowX)->to_string(), longhand(PropertyID::OverflowY)->to_string()));
    case PropertyID::PlaceContent: {
        auto align_content = longhand(PropertyID::AlignContent)->to_string();
        auto justify_content = longhand(PropertyID::JustifyContent)->to_string();
        if (align_content == justify_content)
            return align_content;
        return MUST(String::formatted("{} {}", align_content, justify_content));
    }
    case PropertyID::PlaceItems: {
        auto align_items = longhand(PropertyID::AlignItems)->to_string();
        auto justify_items = longhand(PropertyID::JustifyItems)->to_string();
        if (align_items == justify_items)
            return align_items;
        return MUST(String::formatted("{} {}", align_items, justify_items));
    }
    case PropertyID::PlaceSelf: {
        auto align_self = longhand(PropertyID::AlignSelf)->to_string();
        auto justify_self = longhand(PropertyID::JustifySelf)->to_string();
        if (align_self == justify_self)
            return align_self;
        return MUST(String::formatted("{} {}", align_self, justify_self));
    }
    case PropertyID::TextDecoration:
        return MUST(String::formatted("{} {} {} {}", longhand(PropertyID::TextDecorationLine)->to_string(), longhand(PropertyID::TextDecorationThickness)->to_string(), longhand(PropertyID::TextDecorationStyle)->to_string(), longhand(PropertyID::TextDecorationColor)->to_string()));
    default:
        StringBuilder builder;
        auto first = true;
        for (auto& value : m_properties.values) {
            if (first)
                first = false;
            else
                builder.append(' ');
            builder.append(value->to_string());
        }
        return MUST(builder.to_string());
    }
}

}
