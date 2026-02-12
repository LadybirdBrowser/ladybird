/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ShorthandStyleValue.h"
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
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

void ShorthandStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // If all the longhands are the same CSS-wide keyword, just return that once.
    Optional<Keyword> built_in_keyword;
    bool all_same_keyword = true;
    StyleComputer::for_each_property_expanding_shorthands(m_properties.shorthand_property, *this, [&](PropertyID, StyleValue const& value) {
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
            builder.append(string_from_keyword(built_in_keyword.value()));

        return;
    }

    auto const coordinating_value_list_shorthand_serialize = [&](StringView entry_when_all_longhands_initial, Vector<PropertyID> const& required_longhands = {}, Vector<PropertyID> const& reset_only_longhands = {}) {
        for (auto reset_only_longhand : reset_only_longhands) {
            if (!longhand(reset_only_longhand)->equals(property_initial_value(reset_only_longhand)))
                return;
        }

        auto entry_count = longhand(m_properties.sub_properties[0])->as_value_list().size();

        // If we don't have the same number of values for each non-reset-only longhand, we can't serialize this shorthand.
        if (any_of(m_properties.sub_properties, [&](auto longhand_id) { return !reset_only_longhands.contains_slow(longhand_id) && longhand(longhand_id)->as_value_list().size() != entry_count; }))
            return;

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

        for (size_t entry_index = 0; entry_index < entry_count; entry_index++) {
            bool first = true;

            for (size_t longhand_index = 0; longhand_index < m_properties.sub_properties.size(); longhand_index++) {
                auto longhand_id = m_properties.sub_properties[longhand_index];

                if (!should_serialize_longhand(entry_index, longhand_index))
                    continue;

                if (!builder.is_empty() && !first)
                    builder.append(' ');

                auto longhand_value = longhand(longhand_id)->as_value_list().values()[entry_index];

                longhand_value->serialize(builder, mode);
                first = false;
            }

            if (first)
                builder.append(entry_when_all_longhands_initial);

            if (entry_index != entry_count - 1)
                builder.append(", "sv);
        }
    };

    auto default_serialize = [&]() {
        auto all_properties_same_value = true;
        auto first_property_value = m_properties.values.first();
        for (auto i = 1u; i < m_properties.values.size(); ++i) {
            if (m_properties.values[i] != first_property_value) {
                all_properties_same_value = false;
                break;
            }
        }
        if (all_properties_same_value) {
            first_property_value->serialize(builder, mode);
            return;
        }

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
            builder.append(value_string);
        }
        if (builder.is_empty())
            m_properties.values.first()->serialize(builder, mode);
    };

    // Then special cases
    // FIXME: overflow-clip-margin needs a special case here for when its longhands aren't identical.
    switch (m_properties.shorthand_property) {
    case PropertyID::All: {
        // NOTE: 'all' can only be serialized in the case all sub-properties share the same CSS-wide keyword, this is
        //       handled above, thus, if we get to here that mustn't be the case and we should return the empty string.
        return;
    }
    case PropertyID::Animation:
        coordinating_value_list_shorthand_serialize("none"sv, {}, { PropertyID::AnimationTimeline });
        return;
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

        auto serialize_layer = [mode](StringBuilder& builder, ValueComparingRefPtr<StyleValue const> color_value, ValueComparingRefPtr<StyleValue const> image_value, ValueComparingRefPtr<StyleValue const> position_x_value, ValueComparingRefPtr<StyleValue const> position_y_value, ValueComparingRefPtr<StyleValue const> size_value, ValueComparingRefPtr<StyleValue const> repeat_value, ValueComparingRefPtr<StyleValue const> attachment_value, ValueComparingRefPtr<StyleValue const> origin_value, ValueComparingRefPtr<StyleValue const> clip_value) {
            Vector<PropertyID> property_ids = { PropertyID::BackgroundColor, PropertyID::BackgroundImage, PropertyID::BackgroundPositionX, PropertyID::BackgroundPositionY, PropertyID::BackgroundSize, PropertyID::BackgroundRepeat, PropertyID::BackgroundAttachment, PropertyID::BackgroundOrigin, PropertyID::BackgroundClip };
            Vector<ValueComparingRefPtr<StyleValue const>> property_values = { move(color_value), move(image_value), move(position_x_value), move(position_y_value), move(size_value), move(repeat_value), move(attachment_value), move(origin_value), move(clip_value) };

            bool first = true;
            for (size_t i = 0; i < property_ids.size(); i++) {
                if (!property_values[i])
                    continue;

                auto value_string = property_values[i]->to_string(mode);
                auto initial_value_string = property_initial_value(property_ids[i])->to_string(mode);

                if (value_string != initial_value_string) {
                    if (!first)
                        builder.append(' ');
                    builder.append(value_string);
                    first = false;
                }
            }

            if (first)
                builder.append("none"sv);
        };

        auto get_layer_count = [](auto style_value) -> size_t {
            return style_value->is_value_list() ? style_value->as_value_list().size() : 1;
        };

        auto layer_count = max(get_layer_count(image), max(get_layer_count(position_x), max(get_layer_count(position_y), max(get_layer_count(size), max(get_layer_count(repeat), max(get_layer_count(attachment), max(get_layer_count(origin), get_layer_count(clip))))))));

        if (layer_count == 1) {
            serialize_layer(builder, color, image, position_x, position_y, size, repeat, attachment, origin, clip);
            return;
        }

        auto get_layer_value = [](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) -> ValueComparingRefPtr<StyleValue const> {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true);
            return style_value;
        };

        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);

            ValueComparingRefPtr<StyleValue const> maybe_color_value;
            if (i == layer_count - 1)
                maybe_color_value = color;

            serialize_layer(builder, maybe_color_value, get_layer_value(image, i), get_layer_value(position_x, i), get_layer_value(position_y, i), get_layer_value(size, i), get_layer_value(repeat, i), get_layer_value(attachment, i), get_layer_value(origin, i), get_layer_value(clip, i));
        }
        return;
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
            x_edges->serialize(builder, mode);
            builder.append(' ');
            y_edges->serialize(builder, mode);
            return;
        }

        auto get_layer_value_string = [mode](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) {
            if (style_value->is_value_list())
                return style_value->as_value_list().value_at(index, true)->to_string(mode);
            return style_value->to_string(mode);
        };

        for (size_t i = 0; i < layer_count; i++) {
            if (i)
                builder.append(", "sv);

            builder.appendff("{} {}", get_layer_value_string(x_edges, i), get_layer_value_string(y_edges, i));
        }
        return;
    }
    case PropertyID::Border: {
        // `border` only has a reasonable value if border-image is it's initial value (in which case it is omitted)
        if (!longhand(PropertyID::BorderImage)->equals(property_initial_value(PropertyID::BorderImage)))
            return;

        auto all_longhands_same_value = [](ValueComparingRefPtr<StyleValue const> const& shorthand) -> bool {
            auto longhands = shorthand->as_shorthand().values();

            return all_of(longhands, [&](auto const& longhand) { return longhand == longhands[0]; });
        };

        auto const& border_width = longhand(PropertyID::BorderWidth);
        auto const& border_style = longhand(PropertyID::BorderStyle);
        auto const& border_color = longhand(PropertyID::BorderColor);

        // `border` only has a reasonable value if all four sides are the same.
        if (!all_longhands_same_value(border_width) || !all_longhands_same_value(border_style) || !all_longhands_same_value(border_color))
            return;

        if (!border_width->equals(property_initial_value(PropertyID::BorderWidth)))
            border_width->serialize(builder, mode);

        if (!border_style->equals(property_initial_value(PropertyID::BorderStyle))) {
            if (!builder.is_empty())
                builder.append(' ');
            border_style->serialize(builder, mode);
        }

        if (!border_color->equals(property_initial_value(PropertyID::BorderColor))) {
            if (!builder.is_empty())
                builder.append(' ');
            border_color->serialize(builder, mode);
        }

        if (builder.is_empty())
            border_width->serialize(builder, mode);
        return;
    }
    case PropertyID::BorderImage: {
        auto source = longhand(PropertyID::BorderImageSource);
        auto slice = longhand(PropertyID::BorderImageSlice);
        auto width = longhand(PropertyID::BorderImageWidth);
        auto outset = longhand(PropertyID::BorderImageOutset);
        auto repeat = longhand(PropertyID::BorderImageRepeat);
        source->serialize(builder, mode);
        builder.append(' ');
        slice->serialize(builder, mode);
        builder.append(" / "sv);
        width->serialize(builder, mode);
        builder.append(" / "sv);
        outset->serialize(builder, mode);
        builder.append(' ');
        repeat->serialize(builder, mode);
        return;
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
        if (first_radius_serialization == second_radius_serialization) {
            builder.append(first_radius_serialization);
            return;
        }

        builder.appendff("{} / {}", first_radius_serialization, second_radius_serialization);
        return;
    }
    case PropertyID::Columns: {
        auto column_width = longhand(PropertyID::ColumnWidth)->to_string(mode);
        auto column_count = longhand(PropertyID::ColumnCount)->to_string(mode);
        auto column_height = longhand(PropertyID::ColumnHeight)->to_string(mode);

        if (column_width == column_count) {
            builder.append(column_width);
        } else if (column_width.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(column_count);
        } else if (column_count.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(column_width);
        } else {
            builder.appendff("{} {}", column_width, column_count);
        }

        if (!column_height.equals_ignoring_ascii_case("auto"sv)) {
            builder.append(" / "sv);
            builder.append(column_height);
        }
        return;
    }
    case PropertyID::Flex:
        longhand(PropertyID::FlexGrow)->serialize(builder, mode);
        builder.append(' ');
        longhand(PropertyID::FlexShrink)->serialize(builder, mode);
        builder.append(' ');
        longhand(PropertyID::FlexBasis)->serialize(builder, mode);
        return;
    case PropertyID::Font: {
        auto font_style = longhand(PropertyID::FontStyle);
        auto font_variant = longhand(PropertyID::FontVariant);
        auto font_weight = longhand(PropertyID::FontWeight);
        auto font_width = longhand(PropertyID::FontWidth);
        auto font_size = longhand(PropertyID::FontSize);
        auto line_height = longhand(PropertyID::LineHeight);
        auto font_family = longhand(PropertyID::FontFamily);

        for (auto const& reset_only_sub_property : { PropertyID::FontFeatureSettings, PropertyID::FontKerning, PropertyID::FontLanguageOverride, PropertyID::FontOpticalSizing, PropertyID::FontVariationSettings }) {
            auto const& value = longhand(reset_only_sub_property);

            if (!value->equals(property_initial_value(reset_only_sub_property)))
                return;
        }

        // Some longhands prevent serialization if they are not allowed in the shorthand.
        // <font-variant-css2> = normal | small-caps
        auto font_variant_string = font_variant->to_string(mode);
        if (!first_is_one_of(font_variant_string, "normal"sv, "small-caps"sv) && !CSS::is_css_wide_keyword(font_variant_string)) {
            return;
        }

        // <font-width-css3> = normal | ultra-condensed | extra-condensed | condensed | semi-condensed | semi-expanded | expanded | extra-expanded | ultra-expanded
        auto font_width_as_keyword = [&]() -> Optional<Keyword> {
            if (first_is_one_of(font_width->to_keyword(), Keyword::Normal, Keyword::UltraCondensed, Keyword::ExtraCondensed, Keyword::Condensed, Keyword::SemiCondensed, Keyword::SemiExpanded, Keyword::Expanded, Keyword::ExtraExpanded, Keyword::UltraExpanded))
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
            return;

        auto append = [&](auto const& string) {
            if (!builder.is_empty())
                builder.append(' ');
            builder.append(string);
        };
        auto font_style_string = font_style->to_string(mode);
        if (font_style_string != "normal"sv)
            append(font_style_string);
        if (font_variant_string != "normal"sv)
            append(font_variant_string);
        auto font_weight_string = font_weight->to_string(mode);
        if (font_weight_string != "normal"sv && font_weight_string != "400"sv)
            append(font_weight_string);
        if (font_width_as_keyword != Keyword::Normal)
            append(string_from_keyword(font_width_as_keyword.value()));
        append(font_size->to_string(mode));
        if (line_height->to_keyword() != Keyword::Normal)
            append(MUST(String::formatted("/ {}", line_height->to_string(mode))));
        append(font_family->to_string(mode));
        return;
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
            return;

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

        if (values.is_empty()) {
            builder.append("normal"sv);
            return;
        }
        builder.append(MUST(String::join(' ', values)));
        return;
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

        auto serialize_grid_area = [&]() {
            if (first_is_equal_to_all_of(row_start, column_start, row_end, column_end)) {
                row_start->serialize(builder, mode);
                return;
            }
            if (row_start == row_end && column_start == column_end) {
                row_start->serialize(builder, mode);
                builder.append(" / "sv);
                column_start->serialize(builder, mode);
                return;
            }
            if (column_start == column_end) {
                if (is_auto(row_end)) {
                    if (is_auto(column_start)) {
                        row_start->serialize(builder, mode);
                        return;
                    }
                    row_start->serialize(builder, mode);
                    builder.append(" / "sv);
                    column_start->serialize(builder, mode);
                    return;
                }
                row_start->serialize(builder, mode);
                builder.append(" / "sv);
                column_start->serialize(builder, mode);
                builder.append(" / "sv);
                row_end->serialize(builder, mode);
                return;
            }
            row_start->serialize(builder, mode);
            builder.append(" / "sv);
            column_start->serialize(builder, mode);
            builder.append(" / "sv);
            row_end->serialize(builder, mode);
            builder.append(" / "sv);
            column_end->serialize(builder, mode);
        };

        // If four <grid-line> values are specified, grid-row-start is set to the first value, grid-column-start is set
        // to the second value, grid-row-end is set to the third value, and grid-column-end is set to the fourth value.
        if (!is_auto(row_start) && !is_auto(column_start) && !is_auto(row_end) && !is_auto(column_end)) {
            serialize_grid_area();
            return;
        }

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

        serialize_grid_area();
        return;
    }
    case PropertyID::Grid: {
        // https://drafts.csswg.org/css-grid/#propdef-grid
        // <'grid-template'> |
        // <'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>? |
        // [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>
        auto auto_flow_value = longhand(PropertyID::GridAutoFlow);
        auto auto_rows_value = longhand(PropertyID::GridAutoRows);
        auto auto_columns_value = longhand(PropertyID::GridAutoColumns);

        auto is_initial = [](ValueComparingRefPtr<StyleValue const> const& value, PropertyID property) {
            return *value == *property_initial_value(property);
        };

        bool auto_flow_is_initial = is_initial(auto_flow_value, PropertyID::GridAutoFlow);
        bool auto_rows_is_initial = is_initial(auto_rows_value, PropertyID::GridAutoRows);
        bool auto_columns_is_initial = is_initial(auto_columns_value, PropertyID::GridAutoColumns);

        if (!auto_flow_is_initial || !auto_rows_is_initial || !auto_columns_is_initial) {
            auto areas_value = longhand(PropertyID::GridTemplateAreas);
            auto rows_value = longhand(PropertyID::GridTemplateRows);
            auto columns_value = longhand(PropertyID::GridTemplateColumns);

            bool areas_is_initial = is_initial(areas_value, PropertyID::GridTemplateAreas);
            bool rows_is_initial = is_initial(rows_value, PropertyID::GridTemplateRows);
            bool columns_is_initial = is_initial(columns_value, PropertyID::GridTemplateColumns);

            auto& auto_flow = auto_flow_value->as_grid_auto_flow();

            // [ auto-flow && dense? ] <'grid-auto-rows'>? / <'grid-template-columns'>
            if (auto_flow.is_row() && auto_columns_is_initial && areas_is_initial && rows_is_initial) {
                builder.append("auto-flow"sv);
                if (auto_flow.is_dense())
                    builder.append(" dense"sv);
                if (!auto_rows_is_initial) {
                    builder.append(' ');
                    auto_rows_value->serialize(builder, mode);
                }
                builder.append(" / "sv);
                columns_value->serialize(builder, mode);
                return;
            }

            // <'grid-template-rows'> / [ auto-flow && dense? ] <'grid-auto-columns'>?
            if (auto_flow.is_column() && auto_rows_is_initial && areas_is_initial && columns_is_initial) {
                rows_value->serialize(builder, mode);
                builder.append(" / auto-flow"sv);
                if (auto_flow.is_dense())
                    builder.append(" dense"sv);
                if (!auto_columns_is_initial) {
                    builder.append(' ');
                    auto_columns_value->serialize(builder, mode);
                }
                return;
            }

            return;
        }

        // <'grid-template'>
        [[fallthrough]];
    }
    case PropertyID::GridTemplate: {
        auto areas_value = longhand(PropertyID::GridTemplateAreas);
        auto rows_value = longhand(PropertyID::GridTemplateRows);
        auto columns_value = longhand(PropertyID::GridTemplateColumns);

        if (!areas_value->is_grid_template_area()
            || !rows_value->is_grid_track_size_list()
            || !columns_value->is_grid_track_size_list()) {
            default_serialize();
            return;
        }

        auto& areas = areas_value->as_grid_template_area();
        auto& rows = rows_value->as_grid_track_size_list();
        auto& columns = columns_value->as_grid_track_size_list();

        if (areas.grid_template_area().size() == 0 && rows.grid_track_size_list().track_list().size() == 0 && columns.grid_track_size_list().track_list().size() == 0) {
            builder.append("none"sv);
            return;
        }

        auto construct_rows_string = [&]() {
            StringBuilder inner_builder;
            size_t area_index = 0;
            for (size_t i = 0; i < rows.grid_track_size_list().list().size(); ++i) {
                auto track_size_or_line_names = rows.grid_track_size_list().list()[i];
                if (auto* line_names = track_size_or_line_names.get_pointer<GridLineNames>()) {
                    if (i != 0)
                        inner_builder.append(' ');
                    line_names->serialize(inner_builder);
                }
                if (auto* track_size = track_size_or_line_names.get_pointer<ExplicitGridTrack>()) {
                    if (area_index < areas.grid_template_area().size()) {
                        if (!inner_builder.is_empty())
                            inner_builder.append(' ');
                        inner_builder.append("\""sv);
                        for (size_t y = 0; y < areas.grid_template_area()[area_index].size(); ++y) {
                            if (y != 0)
                                inner_builder.append(' ');
                            inner_builder.append(areas.grid_template_area()[area_index][y]);
                        }
                        inner_builder.append("\""sv);
                    }
                    auto track_size_serialization = track_size->to_string(mode);
                    if (track_size_serialization != "auto"sv) {
                        if (!inner_builder.is_empty())
                            inner_builder.append(' ');
                        inner_builder.append(track_size_serialization);
                    }
                    ++area_index;
                }
            }
            return MUST(inner_builder.to_string());
        };

        if (areas.grid_template_area().is_empty()) {
            rows.grid_track_size_list().serialize(builder, mode);
            builder.append(" / "sv);
            columns.grid_track_size_list().serialize(builder, mode);
            return;
        }

        auto rows_serialization = construct_rows_string();
        if (rows_serialization.is_empty())
            return;

        if (columns.grid_track_size_list().is_empty()) {
            builder.append(rows_serialization);
            return;
        }
        builder.append(construct_rows_string());
        builder.append(" / "sv);
        columns.grid_track_size_list().serialize(builder, mode);
        return;
    }
    case PropertyID::GridColumn: {
        auto start = longhand(PropertyID::GridColumnStart);
        auto end = longhand(PropertyID::GridColumnEnd);
        if (end->as_grid_track_placement().grid_track_placement().is_auto() || start == end) {
            start->serialize(builder, mode);
            return;
        }
        start->serialize(builder, mode);
        builder.append(" / "sv);
        end->serialize(builder, mode);
        return;
    }
    case PropertyID::GridRow: {
        auto start = longhand(PropertyID::GridRowStart);
        auto end = longhand(PropertyID::GridRowEnd);
        if (end->as_grid_track_placement().grid_track_placement().is_auto() || start == end) {
            start->serialize(builder, mode);
            return;
        }
        start->serialize(builder, mode);
        builder.append(" / "sv);
        end->serialize(builder, mode);
        return;
    }
    case PropertyID::Mask: {
        auto serialize_layer = [mode](StringBuilder& builder, ValueComparingRefPtr<StyleValue const> image_value, ValueComparingRefPtr<StyleValue const> position_value, ValueComparingRefPtr<StyleValue const> size_value, ValueComparingRefPtr<StyleValue const> repeat_value, ValueComparingRefPtr<StyleValue const> origin_value, ValueComparingRefPtr<StyleValue const> clip_value, ValueComparingRefPtr<StyleValue const> composite_value, ValueComparingRefPtr<StyleValue const> mode_value) {
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

            Vector<PropertyID> property_ids = { PropertyID::MaskImage, PropertyID::MaskPosition, PropertyID::MaskSize, PropertyID::MaskRepeat, PropertyID::MaskOrigin, PropertyID::MaskClip, PropertyID::MaskComposite, PropertyID::MaskMode };
            Vector<ValueComparingRefPtr<StyleValue const>> property_values = { move(image_value), move(position_value), move(size_value), move(repeat_value), move(origin_value), move(clip_value), move(composite_value), move(mode_value) };

            auto property_value_string = [&](PropertyID property) -> String {
                for (size_t i = 0; i < property_ids.size(); i++) {
                    if (property_ids[i] == property)
                        return property_values[i]->to_string(mode);
                }
                VERIFY_NOT_REACHED();
            };

            auto is_initial_value = [&](PropertyID property) -> bool {
                return property_value_string(property) == property_initial_value(property)->to_string(mode);
            };

            auto can_skip_serializing_initial_value = [&](PropertyID property) -> bool {
                switch (property) {
                case PropertyID::MaskPosition:
                    return is_initial_value(PropertyID::MaskSize);
                case PropertyID::MaskOrigin:
                    return is_initial_value(PropertyID::MaskClip) || property_value_string(PropertyID::MaskClip) == string_from_keyword(Keyword::NoClip);
                default:
                    return true;
                }
            };

            bool layer_is_empty = true;
            for (size_t i = 0; i < array_size(canonical_property_order); i++) {
                auto property = canonical_property_order[i];
                auto value = property_value_string(property);

                if (is_initial_value(property) && can_skip_serializing_initial_value(property))
                    continue;
                if (property == PropertyID::MaskClip && value == property_value_string(PropertyID::MaskOrigin))
                    continue;

                if (!layer_is_empty)
                    builder.append(' ');
                builder.append(value);
                if (property == PropertyID::MaskPosition && !is_initial_value(PropertyID::MaskSize)) {
                    builder.append(" / "sv);
                    builder.append(property_value_string(PropertyID::MaskSize));
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
            serialize_layer(builder, mask_image, mask_position, mask_size, mask_repeat, mask_origin, mask_clip, mask_composite, mask_mode);
        } else {
            auto get_layer_value = [](ValueComparingRefPtr<StyleValue const> const& style_value, size_t index) -> ValueComparingRefPtr<StyleValue const> {
                if (style_value->is_value_list())
                    return style_value->as_value_list().value_at(index, true);
                return style_value;
            };

            for (size_t i = 0; i < layer_count; i++) {
                if (i)
                    builder.append(", "sv);

                serialize_layer(builder, get_layer_value(mask_image, i), get_layer_value(mask_position, i), get_layer_value(mask_size, i), get_layer_value(mask_repeat, i), get_layer_value(mask_origin, i), get_layer_value(mask_clip, i), get_layer_value(mask_composite, i), get_layer_value(mask_mode, i));
            }
        }
        return;
    }
    case PropertyID::PlaceContent:
    case PropertyID::PlaceItems:
    case PropertyID::PlaceSelf:
        builder.append(serialize_a_positional_value_list(m_properties.values, mode));
        return;
    case PropertyID::ScrollTimeline:
        // NB: We don't need to specify a value to use when the entry is empty as all values are initial since
        //     scroll-timeline-name is always included
        coordinating_value_list_shorthand_serialize(""sv, { PropertyID::ScrollTimelineName });
        return;
    case PropertyID::TextDecoration: {
        // The rule here seems to be, only print what's different from the default value,
        // but if they're all default, print the line.
        auto append_if_non_default = [&](PropertyID property_id) {
            auto value = longhand(property_id);
            if (!value->equals(property_initial_value(property_id))) {
                if (!builder.is_empty())
                    builder.append(' ');
                value->serialize(builder, mode);
            }
        };

        append_if_non_default(PropertyID::TextDecorationLine);
        append_if_non_default(PropertyID::TextDecorationThickness);
        append_if_non_default(PropertyID::TextDecorationStyle);
        append_if_non_default(PropertyID::TextDecorationColor);

        if (builder.is_empty())
            longhand(PropertyID::TextDecorationLine)->serialize(builder, mode);

        return;
    }
    case PropertyID::Transition:
        coordinating_value_list_shorthand_serialize("all"sv);
        return;
    case PropertyID::ViewTimeline:
        // NB: We don't need to specify a value to use when the entry is empty as all values are initial since
        //     view-timeline-name is always included
        coordinating_value_list_shorthand_serialize(""sv, { PropertyID::ViewTimelineName });
        return;
    case PropertyID::WhiteSpace: {
        auto white_space_collapse_property = longhand(PropertyID::WhiteSpaceCollapse);
        auto text_wrap_mode_property = longhand(PropertyID::TextWrapMode);
        auto white_space_trim_property = longhand(PropertyID::WhiteSpaceTrim);

        if (white_space_trim_property->is_keyword() && white_space_trim_property->as_keyword().keyword() == Keyword::None) {
            auto white_space_collapse_keyword = white_space_collapse_property->as_keyword().keyword();
            auto text_wrap_mode_keyword = text_wrap_mode_property->as_keyword().keyword();

            if (white_space_collapse_keyword == Keyword::Collapse && text_wrap_mode_keyword == Keyword::Wrap) {
                builder.append("normal"sv);
                return;
            }

            if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Nowrap) {
                builder.append("pre"sv);
                return;
            }

            if (white_space_collapse_keyword == Keyword::Preserve && text_wrap_mode_keyword == Keyword::Wrap) {
                builder.append("pre-wrap"sv);
                return;
            }

            if (white_space_collapse_keyword == Keyword::PreserveBreaks && text_wrap_mode_keyword == Keyword::Wrap) {
                builder.append("pre-line"sv);
                return;
            }
        }

        default_serialize();
        return;
    }
    default:
        if (property_is_positional_value_list_shorthand(m_properties.shorthand_property)) {
            builder.append(serialize_a_positional_value_list(m_properties.values, mode));
            return;
        }

        default_serialize();
    }
}

void ShorthandStyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    Base::set_style_sheet(style_sheet);
    for (auto& value : m_properties.values)
        const_cast<StyleValue&>(*value).set_style_sheet(style_sheet);
}

}
