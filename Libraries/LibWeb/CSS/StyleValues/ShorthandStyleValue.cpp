/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ShorthandStyleValue.h"
#include <LibGfx/Font/FontWeight.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>

namespace Web::CSS {

ShorthandStyleValue::ShorthandStyleValue(PropertyID shorthand, Vector<PropertyID> sub_properties, Vector<ValueComparingNonnullRefPtr<StyleValue const>> values)
    : StyleValueWithDefaultOperators(Type::Shorthand)
    , m_properties { shorthand, move(sub_properties), move(values) }
{
    if (m_properties.sub_properties.size() != m_properties.values.size()) {
        dbgln("ShorthandStyleValue: sub_properties and values must be the same size! {} != {}", m_properties.sub_properties.size(), m_properties.values.size());
        VERIFY_NOT_REACHED();
    }
}

ShorthandStyleValue::~ShorthandStyleValue() = default;

ValueComparingRefPtr<StyleValue const> ShorthandStyleValue::longhand(PropertyID longhand) const
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
    StyleComputer::for_each_property_expanding_shorthands(m_properties.shorthand_property, *this, [&](PropertyID name, StyleValue const& value) {
        (void)name;
        if (!value.is_css_wide_keyword()) {
            all_same_keyword = false;
            return;
        }
        auto keyword = value.to_keyword();
        if (!built_in_keyword.has_value()) {
            built_in_keyword = keyword;
            return;
        }
        if (built_in_keyword != keyword) {
            all_same_keyword = false;
            return;
        }
    });

    if (built_in_keyword.has_value()) {
        if (all_same_keyword)
            return MUST(String::from_utf8(string_from_keyword(built_in_keyword.value())));

        return ""_string;
    }

    auto const coordinating_value_list_shorthand_to_string = [&](StringView entry_when_all_longhands_initial, Vector<PropertyID> const& required_longhands = {}, Vector<PropertyID> const& reset_only_longhands = {}) {
        for (auto reset_only_longhand : reset_only_longhands) {
            if (!longhand(reset_only_longhand)->equals(property_initial_value(reset_only_longhand)))
                return ""_string;
        }

        auto entry_count = longhand(m_properties.sub_properties[0])->as_value_list().size();

        // If we don't have the same number of values for each non-reset-only longhand, we can't serialize this shorthand.
        if (any_of(m_properties.sub_properties, [&](auto longhand_id) { return !reset_only_longhands.contains_slow(longhand_id) && longhand(longhand_id)->as_value_list().size() != entry_count; }))
            return ""_string;

        // We should serialize a longhand if it is not a reset-only longhand and one of the following is true:
        // - The longhand is required
        // - The value is not the initial value
        // - Another longhand value which will be included later in the serialization is valid for this longhand.
        auto should_serialize_longhand = [&](size_t entry_index, size_t longhand_index) {
            auto longhand_id = m_properties.sub_properties[longhand_index];

            if (reset_only_longhands.contains_slow(longhand_id))
                return false;

            if (required_longhands.contains_slow(longhand_id))
                return true;

            auto longhand_value = longhand(longhand_id)->as_value_list().values()[entry_index];

            if (!longhand_value->equals(property_initial_value(longhand_id)->as_value_list().values()[0]))
                return true;

            for (size_t other_longhand_index = longhand_index + 1; other_longhand_index < m_properties.sub_properties.size(); other_longhand_index++) {
                auto other_longhand_id = m_properties.sub_properties[other_longhand_index];

                if (reset_only_longhands.contains_slow(other_longhand_id))
                    continue;

                auto other_longhand_value = longhand(other_longhand_id)->as_value_list().values()[entry_index];

                // FIXME: This should really account for the other longhand being included in the serialization for any reason, not just because it is not the initial value.
                if (other_longhand_value->equals(property_initial_value(other_longhand_id)->as_value_list().values()[0]))
                    continue;

                if (parse_css_value(Parser::ParsingParams {}, other_longhand_value->to_string(mode), longhand_id))
                    return true;
            }

            return false;
        };

        StringBuilder builder;
        for (size_t entry_index = 0; entry_index < entry_count; entry_index++) {
            bool first = true;

            for (size_t longhand_index = 0; longhand_index < m_properties.sub_properties.size(); longhand_index++) {
                auto longhand_id = m_properties.sub_properties[longhand_index];

                if (!should_serialize_longhand(entry_index, longhand_index))
                    continue;

                if (!builder.is_empty() && !first)
                    builder.append(' ');

                auto longhand_value = longhand(longhand_id)->as_value_list().values()[entry_index];

                builder.append(longhand_value->to_string(mode));
                first = false;
            }

            if (first)
                builder.append(entry_when_all_longhands_initial);

            if (entry_index != entry_count - 1)
                builder.append(", "sv);
        }

        return builder.to_string_without_validation();
    };

    auto positional_value_list_shorthand_to_string = [&](Vector<ValueComparingNonnullRefPtr<StyleValue const>> values) -> String {
        switch (values.size()) {
        case 2: {
            auto first_property_serialized = values[0]->to_string(mode);
            auto second_property_serialized = values[1]->to_string(mode);

            if (first_property_serialized == second_property_serialized)
                return first_property_serialized;

            return MUST(String::formatted("{} {}", first_property_serialized, second_property_serialized));
        }
        case 4: {
            auto first_property_serialized = values[0]->to_string(mode);
            auto second_property_serialized = values[1]->to_string(mode);
            auto third_property_serialized = values[2]->to_string(mode);
            auto fourth_property_serialized = values[3]->to_string(mode);

            if (first_is_equal_to_all_of(first_property_serialized, second_property_serialized, third_property_serialized, fourth_property_serialized))
                return first_property_serialized;

            if (first_property_serialized == third_property_serialized && second_property_serialized == fourth_property_serialized)
                return MUST(String::formatted("{} {}", first_property_serialized, second_property_serialized));

            if (second_property_serialized == fourth_property_serialized)
                return MUST(String::formatted("{} {} {}", first_property_serialized, second_property_serialized, third_property_serialized));

            return MUST(String::formatted("{} {} {} {}", first_property_serialized, second_property_serialized, third_property_serialized, fourth_property_serialized));
        }
        default:
            VERIFY_NOT_REACHED();
        }
    };

    auto default_to_string = [&]() {
        auto all_properties_same_value = true;
        auto first_property_value = m_properties.values.first();
        for (auto i = 1u; i < m_properties.values.size(); ++i) {
            if (m_properties.values[i] != first_property_value) {
                all_properties_same_value = false;
                break;
            }
        }
        if (all_properties_same_value)
            return first_property_value->to_string(mode);

        StringBuilder builder;
        auto first = true;
        for (size_t i = 0; i < m_properties.values.size(); ++i) {
            auto value = m_properties.values[i];
            auto value_string = value->to_string(mode);
            auto initial_value_string = property_initial_value(m_properties.sub_properties[i])->to_string(mode);
            if (value_string == initial_value_string)
                continue;
            if (first)
                first = false;
            else
                builder.append(' ');
            builder.append(value->to_string(mode));
        }
        if (builder.is_empty())
            return m_properties.values.first()->to_string(mode);

        return MUST(builder.to_string());
    };

    // Then special cases
    switch (m_properties.shorthand_property) {
    case PropertyID::All: {
        // NOTE: 'all' can only be serialized in the case all sub-properties share the same CSS-wide keyword, this is
        //       handled above, thus, if we get to here that mustn't be the case and we should return the empty string.
        return ""_string;
    }
    case PropertyID::Animation:
        return coordinating_value_list_shorthand_to_string("none"sv, {}, { PropertyID::AnimationTimeline });
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

        auto serialize_layer = [mode](Optional<String> color_value_string, String image_value_string, String position_x_value_string, String position_y_value_string, String size_value_string, String repeat_value_string, String attachment_value_string, String origin_value_string, String clip_value_string) {
            StringBuilder builder;

            Vector<PropertyID> property_ids = { PropertyID::BackgroundColor, PropertyID::BackgroundImage, PropertyID::BackgroundPositionX, PropertyID::BackgroundPositionY, PropertyID::BackgroundSize, PropertyID::BackgroundRepeat, PropertyID::BackgroundAttachment, PropertyID::BackgroundOrigin, PropertyID::BackgroundClip };
            Vector<Optional<String>> property_value_strings = { move(color_value_string), move(image_value_string), move(position_x_value_string), move(position_y_value_string), move(size_value_string), move(repeat_value_string), move(attachment_value_string), move(origin_value_string), move(clip_value_string) };

            for (size_t i = 0; i < property_ids.size(); i++) {
                if (!property_value_strings[i].has_value())
                    continue;

                auto intial_property_string_value = property_initial_value(property_ids[i])->to_string(mode);

                if (property_value_strings[i].value() != intial_property_string_value) {
                    if (!builder.is_empty())
                        builder.append(" "sv);
                    builder.append(property_value_strings[i].value());
                }
            }

            if (builder.is_empty())
                return "none"_string;

            return builder.to_string_without_validation();
        };

        auto get_layer_count = [](auto style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        auto layer_count = max(get_layer_count(image), max(get_layer_count(position_x), max(get_layer_count(position_y), max(get_layer_count(size), max(get_layer_count(repeat), max(get_layer_count(attachment), max(get_layer_count(origin), get_layer_count(clip))))))));

        if (layer_count == 1) {
            return serialize_layer(color->to_string(mode), image->to_string(mode), position_x->to_string(mode), position_y->to_string(mode), size->to_string(mode), repeat->to_string(mode), attachment->to_string(mode), origin->to_string(mode), clip->to_string(mode));
        }

        auto get_layer_value_string = [mode](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true)->to_string(mode);
            return style_value->to_string(mode);
        };

        StringBuilder builder;
        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);

            Optional<String> maybe_color_value_string;
            if (i == layer_count - 1)
                maybe_color_value_string = color->to_string(mode);

            builder.append(serialize_layer(maybe_color_value_string, get_layer_value_string(image, i), get_layer_value_string(position_x, i), get_layer_value_string(position_y, i), get_layer_value_string(size, i), get_layer_value_string(repeat, i), get_layer_value_string(attachment, i), get_layer_value_string(origin, i), get_layer_value_string(clip, i)));
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

        auto get_layer_value_string = [mode](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) {
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
    case PropertyID::Border: {
        // `border` only has a reasonable value if border-image is it's initial value (in which case it is omitted)
        if (!longhand(PropertyID::BorderImage)->equals(property_initial_value(PropertyID::BorderImage)))
            return ""_string;

        auto all_longhands_same_value = [](ValueComparingRefPtr<StyleValue const> const& shorthand) -> bool {
            auto longhands = shorthand->as_shorthand().values();

            return all_of(longhands, [&](auto const& longhand) { return longhand == longhands[0]; });
        };

        auto const& border_width = longhand(PropertyID::BorderWidth);
        auto const& border_style = longhand(PropertyID::BorderStyle);
        auto const& border_color = longhand(PropertyID::BorderColor);

        // `border` only has a reasonable value if all four sides are the same.
        if (!all_longhands_same_value(border_width) || !all_longhands_same_value(border_style) || !all_longhands_same_value(border_color))
            return ""_string;

        StringBuilder builder;

        if (!border_width->equals(property_initial_value(PropertyID::BorderWidth)))
            builder.appendff("{}", border_width->to_string(mode));

        if (!border_style->equals(property_initial_value(PropertyID::BorderStyle))) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.appendff("{}", border_style->to_string(mode));
        }

        if (!border_color->equals(property_initial_value(PropertyID::BorderColor))) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.appendff("{}", border_color->to_string(mode));
        }

        if (builder.is_empty())
            return border_width->to_string(mode);

        return builder.to_string_without_validation();
    }
    case PropertyID::BorderImage: {
        auto source = longhand(PropertyID::BorderImageSource);
        auto slice = longhand(PropertyID::BorderImageSlice);
        auto width = longhand(PropertyID::BorderImageWidth);
        auto outset = longhand(PropertyID::BorderImageOutset);
        auto repeat = longhand(PropertyID::BorderImageRepeat);
        return MUST(String::formatted("{} {} / {} / {} {}",
            source->to_string(mode),
            slice->to_string(mode),
            width->to_string(mode),
            outset->to_string(mode),
            repeat->to_string(mode)));
    }
    case PropertyID::BorderRadius: {
        auto top_left = longhand(PropertyID::BorderTopLeftRadius);
        auto top_right = longhand(PropertyID::BorderTopRightRadius);
        auto bottom_right = longhand(PropertyID::BorderBottomRightRadius);
        auto bottom_left = longhand(PropertyID::BorderBottomLeftRadius);

        auto horizontal_radius = [&](auto& style_value) -> String {
            if (style_value->is_border_radius())
                return style_value->as_border_radius().horizontal_radius()->to_string(mode);
            return style_value->to_string(mode);
        };

        auto top_left_horizontal_string = horizontal_radius(top_left);
        auto top_right_horizontal_string = horizontal_radius(top_right);
        auto bottom_right_horizontal_string = horizontal_radius(bottom_right);
        auto bottom_left_horizontal_string = horizontal_radius(bottom_left);

        auto vertical_radius = [&](auto& style_value) -> String {
            if (style_value->is_border_radius())
                return style_value->as_border_radius().vertical_radius()->to_string(mode);
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
        auto column_height = longhand(PropertyID::ColumnHeight)->to_string(mode);

        StringBuilder builder;

        if (column_width == column_count) {
            builder.append(column_width);
        } else if (column_width.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(column_count);
        } else if (column_count.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(column_width);
        } else {
            builder.append(MUST(String::formatted("{} {}", column_width, column_count)));
        }

        if (!column_height.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(" / "sv);
            builder.append(column_height);
        }

        return builder.to_string_without_validation();
    }
    case PropertyID::Flex:
        return MUST(String::formatted("{} {} {}", longhand(PropertyID::FlexGrow)->to_string(mode), longhand(PropertyID::FlexShrink)->to_string(mode), longhand(PropertyID::FlexBasis)->to_string(mode)));
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
        auto font_width_as_keyword = [&]() -> Optional<Keyword> {
            if (first_is_one_of(font_width->to_keyword(), Keyword::Initial, Keyword::Normal, Keyword::UltraCondensed, Keyword::ExtraCondensed, Keyword::Condensed, Keyword::SemiCondensed, Keyword::SemiExpanded, Keyword::Expanded, Keyword::ExtraExpanded, Keyword::UltraExpanded))
                return font_width->to_keyword();

            Optional<double> font_width_as_percentage;

            if (font_width->is_percentage())
                font_width_as_percentage = font_width->as_percentage().raw_value();
            else if (font_width->is_calculated())
                // NOTE: We don't pass a length resolution context but that's fine because either:
                //  - We are working with declarations in which case relative units can't be mapped so their mere
                //    presence means we can't serialize this font shorthand
                //  - We are working with computed values in which case we would have already converted any
                //    CalculatedStyleValues values to normal PercentageStyleValues
                font_width_as_percentage = font_width->as_calculated().resolve_percentage({}).map([](auto percentage) { return percentage.value(); });

            if (!font_width_as_percentage.has_value())
                return {};

            // ultra-condensed 50%
            if (font_width_as_percentage == 50)
                return Keyword::UltraCondensed;

            // extra-condensed 62.5%
            if (font_width_as_percentage == 62.5)
                return Keyword::ExtraCondensed;

            // condensed 75%
            if (font_width_as_percentage == 75)
                return Keyword::Condensed;

            // semi-condensed 87.5%
            if (font_width_as_percentage == 87.5)
                return Keyword::SemiCondensed;

            // normal 100%
            if (font_width_as_percentage == 100)
                return Keyword::Normal;

            // semi-expanded 112.5%
            if (font_width_as_percentage == 112.5)
                return Keyword::SemiExpanded;

            // expanded 125%
            if (font_width_as_percentage == 125)
                return Keyword::Expanded;

            // extra-expanded 150%
            if (font_width_as_percentage == 150)
                return Keyword::ExtraExpanded;

            // ultra-expanded 200%
            if (font_width_as_percentage == 200)
                return Keyword::UltraExpanded;

            return {};
        }();

        if (!font_width_as_keyword.has_value())
            return {};

        StringBuilder builder;
        auto append = [&](auto const& string) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.append(string);
        };
        auto font_style_string = font_style->to_string(mode);
        if (font_style_string != "normal"sv)
            append(font_style_string);
        if (font_variant_string != "normal"sv && font_variant_string != "initial"sv)
            append(font_variant_string);
        auto font_weight_string = font_weight->to_string(mode);
        if (font_weight_string != "normal"sv && font_weight_string != "initial"sv && font_weight_string != "400"sv)
            append(font_weight_string);
        if (font_width_as_keyword != Keyword::Normal && font_width_as_keyword != Keyword::Initial)
            append(string_from_keyword(font_width_as_keyword.value()));
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

        // If ligatures is `none` and any other value isn't `normal`, that's invalid.
        if (ligatures->to_keyword() == Keyword::None && !first_is_equal_to_all_of(Keyword::Normal, caps->to_keyword(), alternates->to_keyword(), numeric->to_keyword(), east_asian->to_keyword(), position->to_keyword(), emoji->to_keyword()))
            return ""_string;

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
        // https://drafts.csswg.org/css-grid/#propdef-grid-area
        // The grid-area property is a shorthand for grid-row-start, grid-column-start, grid-row-end and grid-column-end.
        auto row_start = longhand(PropertyID::GridRowStart);
        auto column_start = longhand(PropertyID::GridColumnStart);
        auto row_end = longhand(PropertyID::GridRowEnd);
        auto column_end = longhand(PropertyID::GridColumnEnd);
        auto is_auto = [](auto const& track_placement) {
            if (track_placement->is_grid_track_placement())
                return track_placement->as_grid_track_placement().grid_track_placement().is_auto();
            return false;
        };

        auto serialize_values = [&]() {
            if (first_is_equal_to_all_of(row_start, column_start, row_end, column_end))
                return row_start->to_string(mode);
            if (row_start == row_end && column_start == column_end)
                return MUST(String::formatted("{} / {}", row_start->to_string(mode), column_start->to_string(mode)));
            if (column_start == column_end) {
                if (is_auto(row_end)) {
                    if (is_auto(column_start))
                        return row_start->to_string(mode);
                    return MUST(String::formatted("{} / {}", row_start->to_string(mode), column_start->to_string(mode)));
                }
                return MUST(String::formatted("{} / {} / {}", row_start->to_string(mode), column_start->to_string(mode), row_end->to_string(mode)));
            }
            return MUST(String::formatted("{} / {} / {} / {}", row_start->to_string(mode), column_start->to_string(mode), row_end->to_string(mode), column_end->to_string(mode)));
        };

        // If four <grid-line> values are specified, grid-row-start is set to the first value, grid-column-start is set
        // to the second value, grid-row-end is set to the third value, and grid-column-end is set to the fourth value.
        if (!is_auto(row_start) && !is_auto(column_start) && !is_auto(row_end) && !is_auto(column_end))
            return serialize_values();

        // When grid-column-end is omitted, if grid-column-start is a <custom-ident>, grid-column-end is set to that
        // <custom-ident>; otherwise, it is set to auto.
        if (is_auto(column_end) && column_start->is_custom_ident())
            column_end = column_start;

        // When grid-column-start is omitted, if grid-row-start is a <custom-ident>, all four longhands are set to
        // that value. Otherwise, it is set to auto.
        if (is_auto(column_start) && row_start->is_custom_ident()) {
            column_start = row_start;
            row_end = row_start;
            column_end = row_start;
        }

        // When grid-row-end is omitted, if grid-row-start is a <custom-ident>, grid-row-end is set to that
        // <custom-ident>; otherwise, it is set to auto.
        if (is_auto(row_end) && row_start->is_custom_ident())
            row_end = row_start;

        return serialize_values();
    }
        // FIXME: Serialize Grid differently once we support it better!
    case PropertyID::Grid:
    case PropertyID::GridTemplate: {
        auto areas_value = longhand(PropertyID::GridTemplateAreas);
        auto rows_value = longhand(PropertyID::GridTemplateRows);
        auto columns_value = longhand(PropertyID::GridTemplateColumns);

        if (!areas_value->is_grid_template_area()
            || !rows_value->is_grid_track_size_list()
            || !columns_value->is_grid_track_size_list()) {
            return default_to_string();
        }

        auto& areas = areas_value->as_grid_template_area();
        auto& rows = rows_value->as_grid_track_size_list();
        auto& columns = columns_value->as_grid_track_size_list();

        if (areas.grid_template_area().size() == 0 && rows.grid_track_size_list().track_list().size() == 0 && columns.grid_track_size_list().track_list().size() == 0)
            return "none"_string;

        auto construct_rows_string = [&]() {
            StringBuilder builder;
            for (size_t i = 0; i < rows.grid_track_size_list().list().size(); ++i) {
                auto track_size_or_line_names = rows.grid_track_size_list().list()[i];
                if (auto* line_names = track_size_or_line_names.get_pointer<GridLineNames>()) {
                    if (i != 0)
                        builder.append(' ');
                    builder.append(line_names->to_string());
                }
                if (areas.grid_template_area().size() > i) {
                    if (!builder.is_empty())
                        builder.append(' ');
                    builder.append("\""sv);
                    for (size_t y = 0; y < areas.grid_template_area()[i].size(); ++y) {
                        if (y != 0)
                            builder.append(' ');
                        builder.append(areas.grid_template_area()[i][y]);
                    }
                    builder.append("\""sv);
                }
                if (auto* track_size = track_size_or_line_names.get_pointer<ExplicitGridTrack>()) {
                    auto track_size_serialization = track_size->to_string(mode);
                    if (track_size_serialization != "auto"sv) {
                        if (!builder.is_empty())
                            builder.append(' ');
                        builder.append(track_size_serialization);
                    }
                }
            }
            return MUST(builder.to_string());
        };

        if (areas.grid_template_area().is_empty())
            return MUST(String::formatted("{} / {}", rows.grid_track_size_list().to_string(mode), columns.grid_track_size_list().to_string(mode)));

        auto rows_serialization = construct_rows_string();
        if (rows_serialization.is_empty())
            return String {};

        if (columns.grid_track_size_list().is_empty())
            return MUST(String::formatted("{}", rows_serialization));
        return MUST(String::formatted("{} / {}", construct_rows_string(), columns.grid_track_size_list().to_string(mode)));
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
    case PropertyID::Mask: {
        StringBuilder builder;

        auto serialize_layer = [mode, &builder](String image_value_string, String position_value_string, String size_value_string, String repeat_value_string, String origin_value_string, String clip_value_string, String composite_value_string, String mode_value_string) {
            PropertyID canonical_property_order[] = {
                PropertyID::MaskImage,
                PropertyID::MaskPosition,
                // Intentionally skipping MaskSize here, it is handled together with MaskPosition.
                PropertyID::MaskRepeat,
                PropertyID::MaskOrigin,
                PropertyID::MaskClip,
                PropertyID::MaskComposite,
                PropertyID::MaskMode,
            };

            auto property_value = [&](PropertyID property) -> String const& {
                switch (property) {
                case PropertyID::MaskImage:
                    return image_value_string;
                case PropertyID::MaskPosition:
                    return position_value_string;
                case PropertyID::MaskSize:
                    return size_value_string;
                case PropertyID::MaskRepeat:
                    return repeat_value_string;
                case PropertyID::MaskOrigin:
                    return origin_value_string;
                case PropertyID::MaskClip:
                    return clip_value_string;
                case PropertyID::MaskComposite:
                    return composite_value_string;
                case PropertyID::MaskMode:
                    return mode_value_string;
                default:
                    VERIFY_NOT_REACHED();
                }
            };

            auto is_initial_value = [mode, property_value](PropertyID property) -> bool {
                return property_value(property) == property_initial_value(property)->to_string(mode);
            };

            auto can_skip_serializing_initial_value = [is_initial_value, property_value](PropertyID property) -> bool {
                switch (property) {
                case PropertyID::MaskPosition:
                    return is_initial_value(PropertyID::MaskSize);
                case PropertyID::MaskOrigin:
                    return is_initial_value(PropertyID::MaskClip) || property_value(PropertyID::MaskClip) == string_from_keyword(Keyword::NoClip);
                default:
                    return true;
                }
            };

            bool layer_is_empty = true;
            for (size_t i = 0; i < array_size(canonical_property_order); i++) {
                auto property = canonical_property_order[i];
                auto const& value = property_value(property);

                if (is_initial_value(property) && can_skip_serializing_initial_value(property))
                    continue;
                if (property == PropertyID::MaskClip && value == property_value(PropertyID::MaskOrigin))
                    continue;

                if (!layer_is_empty)
                    builder.append(" "sv);
                builder.append(value);
                if (property == PropertyID::MaskPosition && !is_initial_value(PropertyID::MaskSize)) {
                    builder.append(" / "sv);
                    builder.append(property_value(PropertyID::MaskSize));
                }
                layer_is_empty = false;
            }

            if (layer_is_empty)
                builder.append("none"sv);
        };

        auto get_layer_count = [](auto const& style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        auto mask_image = longhand(PropertyID::MaskImage);
        auto mask_position = longhand(PropertyID::MaskPosition);
        auto mask_size = longhand(PropertyID::MaskSize);
        auto mask_repeat = longhand(PropertyID::MaskRepeat);
        auto mask_origin = longhand(PropertyID::MaskOrigin);
        auto mask_clip = longhand(PropertyID::MaskClip);
        auto mask_composite = longhand(PropertyID::MaskComposite);
        auto mask_mode = longhand(PropertyID::MaskMode);

        auto layer_count = max(get_layer_count(mask_image), max(get_layer_count(mask_position), max(get_layer_count(mask_size), max(get_layer_count(mask_repeat), max(get_layer_count(mask_origin), max(get_layer_count(mask_clip), max(get_layer_count(mask_composite), get_layer_count(mask_mode))))))));

        if (layer_count == 1) {
            serialize_layer(mask_image->to_string(mode), mask_position->to_string(mode), mask_size->to_string(mode), mask_repeat->to_string(mode), mask_origin->to_string(mode), mask_clip->to_string(mode), mask_composite->to_string(mode), mask_mode->to_string(mode));
        } else {
            auto get_layer_value_string = [mode](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) {
                if (style_value->is_value_list())
                    return style_value->as_value_list().value_at(index, true)->to_string(mode);
                return style_value->to_string(mode);
            };

            for (size_t i = 0; i < layer_count; i++) {
                if (i)
                    builder.append(", "sv);

                serialize_layer(get_layer_value_string(mask_image, i), get_layer_value_string(mask_position, i), get_layer_value_string(mask_size, i), get_layer_value_string(mask_repeat, i), get_layer_value_string(mask_origin, i), get_layer_value_string(mask_clip, i), get_layer_value_string(mask_composite, i), get_layer_value_string(mask_mode, i));
            }
        }
        return builder.to_string_without_validation();
    }
    case PropertyID::PlaceContent:
    case PropertyID::PlaceItems:
    case PropertyID::PlaceSelf:
        return positional_value_list_shorthand_to_string(m_properties.values);
    case PropertyID::ScrollTimeline:
        // NB: We don't need to specify a value to use when the entry is empty as all values are initial since
        //     scroll-timeline-name is always included
        return coordinating_value_list_shorthand_to_string(""sv, { PropertyID::ScrollTimelineName });
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
    case PropertyID::Transition:
        return coordinating_value_list_shorthand_to_string("all"sv);
    case PropertyID::ViewTimeline:
        // NB: We don't need to specify a value to use when the entry is empty as all values are initial since
        //     view-timeline-name is always included
        return coordinating_value_list_shorthand_to_string(""sv, { PropertyID::ViewTimelineName });
    case PropertyID::WhiteSpace: {
        auto white_space_collapse_property = longhand(PropertyID::WhiteSpaceCollapse);
        auto text_wrap_mode_property = longhand(PropertyID::TextWrapMode);
        auto white_space_trim_property = longhand(PropertyID::WhiteSpaceTrim);

        if (white_space_trim_property->is_keyword() && white_space_trim_property->as_keyword().keyword() == Keyword::None) {
            auto white_space_collapse_keyword = white_space_collapse_property->as_keyword().keyword();
            auto text_wrap_mode_keyword = text_wrap_mode_property->as_keyword().keyword();

            if (white_space_collapse_keyword == Keyword::Collapse && text_wrap_mode_keyword == Keyword::Wrap)
                return "normal"_string;

            if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Nowrap)
                return "pre"_string;

            if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Wrap)
                return "pre-wrap"_string;

            if (white_space_collapse_keyword == Keyword::PreserveBreaks && text_wrap_mode_keyword == Keyword::Wrap)
                return "pre-line"_string;
        }

        return default_to_string();
    }
    default:
        if (property_is_positional_value_list_shorthand(m_properties.shorthand_property))
            return positional_value_list_shorthand_to_string(m_properties.values);

        return default_to_string();
    }
}

void ShorthandStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    for (auto& value : m_properties.values)
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

}
