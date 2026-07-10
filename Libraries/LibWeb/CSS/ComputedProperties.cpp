/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16StringBuilder.h>
#include <LibCore/DirIterator.h>
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/EmptyOptionalStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

ComputedValues::ComputedValues() = default;
ComputedValues::~ComputedValues() = default;

void ComputedValues::Mutator::set_animated_properties(AnimatedProperties const* value)
{
    m_values.m_animated_properties = value;
}

RefPtr<AnimatedProperties const> ComputedValues::animated_properties_snapshot() const
{
    return m_animated_properties;
}

RefPtr<StyleValue const> ComputedValues::color_style_value() const
{
    if (m_inherited.color_style_value)
        return m_inherited.color_style_value;
    return computed_style_value(PropertyID::Color);
}

static_assert(to_underlying(PseudoElement::KnownPseudoElementCount) <= sizeof(u64) * 8);

RefPtr<StyleValue const> ComputedValues::computed_style_value(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && m_base_values)
        return m_base_values->computed_style_value(property_id);

    if (property_is_logical_alias(property_id))
        property_id = map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { writing_mode(), direction() });

    if (auto inset = anchor_inset(property_id))
        return inset;

    auto color_style_value = [](Color color) {
        return ColorStyleValue::create_from_color(color, ColorSyntax::Modern);
    };
    auto shadow_color_style_value = [](ShadowData const& shadow) -> NonnullRefPtr<StyleValue const> {
        if (shadow.color_syntax == ColorSyntax::Legacy)
            return ColorStyleValue::create_from_color(shadow.color, ColorSyntax::Legacy);
        return ColorFunctionStyleValue::create(
            ColorStyleValue::ColorType::sRGB,
            NumberStyleValue::create(shadow.color.red() / 255.0),
            NumberStyleValue::create(shadow.color.green() / 255.0),
            NumberStyleValue::create(shadow.color.blue() / 255.0),
            NumberStyleValue::create(shadow.color.alpha() / 255.0));
    };
    auto length_style_value = [](CSSPixels length) {
        return LengthStyleValue::create(Length::make_px(length));
    };
    auto border_image_slice_style_value = [](BorderImageSliceValue const& value) -> NonnullRefPtr<StyleValue const> {
        return value.visit(
            [](double number) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(number); },
            [](Percentage percentage) -> NonnullRefPtr<StyleValue const> { return PercentageStyleValue::create(percentage); },
            [](NonnullRefPtr<CalculatedStyleValue const> const& calculated) -> NonnullRefPtr<StyleValue const> { return calculated; });
    };
    auto length_percentage_style_value = [](LengthPercentage const& length_percentage) -> NonnullRefPtr<StyleValue const> {
        if (length_percentage.is_percentage())
            return PercentageStyleValue::create(length_percentage.percentage());
        if (length_percentage.is_length())
            return LengthStyleValue::create(length_percentage.length());
        return length_percentage.calculated();
    };
    auto length_percentage_or_auto_style_value = [](LengthPercentageOrAuto const& length_percentage) -> NonnullRefPtr<StyleValue const> {
        if (length_percentage.is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        if (length_percentage.is_percentage())
            return PercentageStyleValue::create(length_percentage.percentage());
        if (length_percentage.is_length())
            return LengthStyleValue::create(length_percentage.length());
        return length_percentage.calculated();
    };
    auto border_image_width_style_value = [&](BorderImageWidthValue const& value) -> NonnullRefPtr<StyleValue const> {
        return value.visit(
            [](double number) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(number); },
            [&](LengthPercentage const& length_percentage) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(length_percentage); },
            [](BorderImageWidthAuto) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Auto); });
    };
    auto border_image_outset_style_value = [](BorderImageOutsetValue const& value) -> NonnullRefPtr<StyleValue const> {
        return value.visit(
            [](double number) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(number); },
            [](Length const& length) -> NonnullRefPtr<StyleValue const> { return LengthStyleValue::create(length); });
    };
    auto border_image_side_values = [](auto const& sides, u8 value_count, auto const& to_style_value) {
        auto top = to_style_value(sides.top);
        auto right = to_style_value(sides.right);
        auto bottom = to_style_value(sides.bottom);
        auto left = to_style_value(sides.left);
        StyleValueVector values { top };
        if (value_count >= 2)
            values.append(move(right));
        if (value_count >= 3)
            values.append(move(bottom));
        if (value_count >= 4)
            values.append(move(left));
        return values;
    };
    auto computed_content_item_style_value = [](ComputedContentItem const& item) -> NonnullRefPtr<StyleValue const> {
        return item.visit(
            [](Utf16String const& string) -> NonnullRefPtr<StyleValue const> { return StringStyleValue::create(string); },
            [](Keyword keyword) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(keyword); },
            [](ComputedContentCounter const& counter) -> NonnullRefPtr<StyleValue const> {
                auto counter_style = CounterStyleStyleValue::create(counter.style.visit(
                    [](Utf16FlyString const& name) -> Variant<Utf16FlyString, CounterStyleStyleValue::SymbolsFunction> { return name; },
                    [](ComputedContentCounter::SymbolsFunction const& symbols) -> Variant<Utf16FlyString, CounterStyleStyleValue::SymbolsFunction> {
                        return CounterStyleStyleValue::SymbolsFunction { .type = symbols.type, .symbols = symbols.symbols };
                    }));
                if (counter.function == ComputedContentCounter::Function::Counters)
                    return CounterStyleValue::create_counters(counter.name, counter.join_string, move(counter_style));
                return CounterStyleValue::create_counter(counter.name, move(counter_style));
            },
            [](NonnullRefPtr<AbstractImageStyleValue const> const& image) -> NonnullRefPtr<StyleValue const> { return image; });
    };
    auto size_style_value = [&](Size const& size) -> NonnullRefPtr<StyleValue const> {
        if (size.is_none())
            return KeywordStyleValue::create(Keyword::None);
        if (size.is_percentage())
            return PercentageStyleValue::create(size.percentage());
        if (size.is_length())
            return LengthStyleValue::create(size.length());
        if (size.is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        if (size.is_calculated())
            return size.calculated();
        if (size.is_min_content())
            return KeywordStyleValue::create(Keyword::MinContent);
        if (size.is_max_content())
            return KeywordStyleValue::create(Keyword::MaxContent);
        if (auto available_space = size.fit_content_available_space(); available_space.has_value())
            return FunctionStyleValue::create("fit-content"_utf16_fly_string, length_percentage_style_value(available_space.release_value()));
        return KeywordStyleValue::create(Keyword::FitContent);
    };
    auto border_radius_style_value = [&](BorderRadiusData const& border_radius) -> NonnullRefPtr<StyleValue const> {
        return BorderRadiusStyleValue::create(
            length_percentage_style_value(border_radius.horizontal_radius),
            length_percentage_style_value(border_radius.vertical_radius));
    };
    auto position_style_value = [&](Position const& position) -> NonnullRefPtr<StyleValue const> {
        auto horizontal_edge = position.edge_x == PositionEdge::Left ? Optional<PositionEdge> {} : position.edge_x;
        auto vertical_edge = position.edge_y == PositionEdge::Top ? Optional<PositionEdge> {} : position.edge_y;
        return PositionStyleValue::create(
            EdgeStyleValue::create(horizontal_edge, length_percentage_style_value(position.offset_x)),
            EdgeStyleValue::create(vertical_edge, length_percentage_style_value(position.offset_y)));
    };
    auto svg_paint_style_value = [&](Optional<SVGPaint> const& paint) -> NonnullRefPtr<StyleValue const> {
        if (!paint.has_value())
            return KeywordStyleValue::create(Keyword::None);
        if (paint->is_color() && paint->color_is_currentcolor())
            return KeywordStyleValue::create(Keyword::Currentcolor);
        if (paint->is_color())
            return color_style_value(paint->as_color());
        StyleValueVector values { URLStyleValue::create(paint->as_url()) };
        if (paint->color_is_currentcolor())
            values.append(KeywordStyleValue::create(Keyword::Currentcolor));
        else if (paint->fallback_color().has_value())
            values.append(color_style_value(*paint->fallback_color()));
        else
            values.append(EmptyOptionalStyleValue::create());
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    };
    auto filter_style_value = [](Filter const& filter) -> NonnullRefPtr<StyleValue const> {
        if (filter.is_none())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector filters;
        MUST(filters.try_ensure_capacity(filter.filters().size()));
        for (auto const& filter_value : filter.filters())
            filters.unchecked_append(filter_value);
        return StyleValueList::create(move(filters), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
    };
    auto ratio_style_value = [](Ratio const& ratio) -> NonnullRefPtr<StyleValue const> {
        return RatioStyleValue::create(
            NumberStyleValue::create(ratio.numerator()),
            NumberStyleValue::create(ratio.denominator()));
    };
    auto length_or_auto_style_value = [&](LengthOrAuto const& value) -> NonnullRefPtr<StyleValue const> {
        if (value.is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        return LengthStyleValue::create(value.length());
    };
    auto custom_ident_list_style_value = [](Vector<Utf16FlyString> const& names) -> NonnullRefPtr<StyleValue const> {
        StyleValueVector values;
        values.ensure_capacity(names.size());
        for (auto const& name : names)
            values.append(CustomIdentStyleValue::create(name));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    };
    auto position_area_style_value = [](PositionAreaData const& area) -> NonnullRefPtr<StyleValue const> {
        VERIFY(!area.keywords.is_empty());
        if (area.keywords.size() == 1)
            return KeywordStyleValue::create(to_keyword(area.keywords[0]));
        StyleValueVector values;
        for (auto keyword : area.keywords)
            values.append(KeywordStyleValue::create(to_keyword(keyword)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    };
    auto timeline_name_list_style_value = [](Vector<Optional<Utf16FlyString>> const& names) -> NonnullRefPtr<StyleValue const> {
        StyleValueVector values;
        for (auto const& name : names) {
            if (name.has_value())
                values.append(CustomIdentStyleValue::create(*name));
            else
                values.append(KeywordStyleValue::create(Keyword::None));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    };
    auto timeline_axis_list_style_value = [](Vector<Axis> const& axes) -> NonnullRefPtr<StyleValue const> {
        StyleValueVector values;
        for (auto axis : axes)
            values.append(KeywordStyleValue::create(to_keyword(axis)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    };

    switch (property_id) {
    case PropertyID::AccentColor:
        return accent_color_value().computed_value.visit(
            [](AccentColor::Auto) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Auto); },
            [&](Color color) -> NonnullRefPtr<StyleValue const> { return color_style_value(color); });
    case PropertyID::AlignContent:
        return KeywordStyleValue::create(to_keyword(align_content()));
    case PropertyID::AlignItems:
        return KeywordStyleValue::create(to_keyword(align_items()));
    case PropertyID::AlignSelf:
        return KeywordStyleValue::create(to_keyword(align_self()));
    case PropertyID::AnchorName:
        if (anchor_names().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        return custom_ident_list_style_value(anchor_names());
    case PropertyID::AnchorScope:
        if (anchor_scope().all)
            return KeywordStyleValue::create(Keyword::All);
        if (anchor_scope().names.is_empty())
            return KeywordStyleValue::create(Keyword::None);
        return custom_ident_list_style_value(anchor_scope().names);
    case PropertyID::Appearance:
        return KeywordStyleValue::create(to_keyword(computed_appearance()));
    case PropertyID::AspectRatio: {
        if (!aspect_ratio().computed_ratio.has_value())
            return KeywordStyleValue::create(Keyword::Auto);
        auto ratio = ratio_style_value(*aspect_ratio().computed_ratio);
        if (!aspect_ratio().computed_use_natural_aspect_ratio_if_available)
            return ratio;
        return StyleValueList::create(
            { KeywordStyleValue::create(Keyword::Auto), move(ratio) },
            StyleValueList::Separator::Space);
    }
    case PropertyID::BackgroundColor:
        return color_style_value(background_color());
    case PropertyID::BackgroundAttachment: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.attachment)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundBlendMode: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.blend_mode)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundClip: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.clip)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundImage: {
        StyleValueVector values;
        for (auto const& layer : background_layers()) {
            if (layer.background_image)
                values.append(*layer.background_image);
            else
                values.append(KeywordStyleValue::create(Keyword::None));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundOrigin: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.origin)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundPositionX: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(EdgeStyleValue::create({}, length_percentage_style_value(layer.position_x)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundPositionY: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(EdgeStyleValue::create({}, length_percentage_style_value(layer.position_y)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundRepeat: {
        StyleValueVector values;
        for (auto const& layer : background_layers())
            values.append(RepeatStyleStyleValue::create(layer.repeat_x, layer.repeat_y));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackgroundSize: {
        StyleValueVector values;
        for (auto const& layer : background_layers()) {
            switch (layer.size_type) {
            case CSS::BackgroundSize::Contain:
                values.append(KeywordStyleValue::create(Keyword::Contain));
                break;
            case CSS::BackgroundSize::Cover:
                values.append(KeywordStyleValue::create(Keyword::Cover));
                break;
            case CSS::BackgroundSize::LengthPercentage:
                values.append(BackgroundSizeStyleValue::create(
                    length_percentage_or_auto_style_value(layer.size_x),
                    length_percentage_or_auto_style_value(layer.size_y)));
                break;
            }
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BorderCollapse:
        return KeywordStyleValue::create(to_keyword(border_collapse()));
    case PropertyID::BorderImageOutset: {
        auto values = border_image_side_values(border_image().outset, border_image().outset_value_count, border_image_outset_style_value);
        if (values.size() == 1)
            return values.take_first();
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::BorderImageRepeat:
        return StyleValueList::create(
            { KeywordStyleValue::create(to_keyword(border_image().repeat_x)), KeywordStyleValue::create(to_keyword(border_image().repeat_y)) },
            StyleValueList::Separator::Space);
    case PropertyID::BorderImageSlice:
        return BorderImageSliceStyleValue::create(
            border_image_slice_style_value(border_image().slice.top),
            border_image_slice_style_value(border_image().slice.right),
            border_image_slice_style_value(border_image().slice.bottom),
            border_image_slice_style_value(border_image().slice.left),
            border_image().fill);
    case PropertyID::BorderImageSource:
        if (border_image().source)
            return *border_image().source;
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::BorderImageWidth: {
        auto values = border_image_side_values(border_image().width, border_image().width_value_count, border_image_width_style_value);
        if (values.size() == 1)
            return values.take_first();
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::BorderSpacing:
        if (border_spacing_horizontal() == border_spacing_vertical())
            return length_style_value(border_spacing_horizontal());
        return StyleValueList::create(
            { length_style_value(border_spacing_horizontal()), length_style_value(border_spacing_vertical()) },
            StyleValueList::Separator::Space);
    case PropertyID::BorderBottomColor:
        if (m_noninherited.border_bottom_color_style_value && !m_noninherited.border_bottom_color_style_value->depends_on_current_color())
            return m_noninherited.border_bottom_color_style_value;
        return color_style_value(border_bottom().color);
    case PropertyID::BorderLeftColor:
        if (m_noninherited.border_left_color_style_value && !m_noninherited.border_left_color_style_value->depends_on_current_color())
            return m_noninherited.border_left_color_style_value;
        return color_style_value(border_left().color);
    case PropertyID::BorderRightColor:
        if (m_noninherited.border_right_color_style_value && !m_noninherited.border_right_color_style_value->depends_on_current_color())
            return m_noninherited.border_right_color_style_value;
        return color_style_value(border_right().color);
    case PropertyID::BorderTopColor:
        if (m_noninherited.border_top_color_style_value && !m_noninherited.border_top_color_style_value->depends_on_current_color())
            return m_noninherited.border_top_color_style_value;
        return color_style_value(border_top().color);
    case PropertyID::CaretColor:
        return color_style_value(caret_color());
    case PropertyID::CaptionSide:
        return KeywordStyleValue::create(to_keyword(caption_side()));
    case PropertyID::ClipRule:
        return KeywordStyleValue::create(to_keyword(clip_rule()));
    case PropertyID::ColorInterpolation:
        return KeywordStyleValue::create(to_keyword(color_interpolation()));
    case PropertyID::ColorInterpolationFilters:
        return KeywordStyleValue::create(to_keyword(color_interpolation_filters()));
    case PropertyID::ColumnHeight:
        return size_style_value(column_height());
    case PropertyID::ColumnSpan:
        return KeywordStyleValue::create(to_keyword(column_span()));
    case PropertyID::ColumnWidth:
        return size_style_value(column_width());
    case PropertyID::Color:
        if (m_inherited.color_style_value && !m_inherited.color_style_value->depends_on_current_color())
            return m_inherited.color_style_value;
        return color_style_value(color());
    case PropertyID::FloodColor:
        return color_style_value(flood_color());
    case PropertyID::StopColor:
        return color_style_value(stop_color());
    case PropertyID::TextDecorationColor:
        return color_style_value(text_decoration_color());
    case PropertyID::WebkitTextFillColor:
        if (webkit_text_fill_color_is_current_color())
            return KeywordStyleValue::create(Keyword::Currentcolor);
        return color_style_value(webkit_text_fill_color());
    case PropertyID::BorderBottomStyle:
        return KeywordStyleValue::create(to_keyword(border_bottom().line_style));
    case PropertyID::BorderLeftStyle:
        return KeywordStyleValue::create(to_keyword(border_left().line_style));
    case PropertyID::BorderRightStyle:
        return KeywordStyleValue::create(to_keyword(border_right().line_style));
    case PropertyID::BorderTopStyle:
        return KeywordStyleValue::create(to_keyword(border_top().line_style));
    case PropertyID::BorderBottomWidth:
        return length_style_value(border_bottom_computed_width());
    case PropertyID::BorderLeftWidth:
        return length_style_value(border_left_computed_width());
    case PropertyID::BorderRightWidth:
        return length_style_value(border_right_computed_width());
    case PropertyID::BorderTopWidth:
        return length_style_value(border_top_computed_width());
    case PropertyID::BorderBottomLeftRadius:
        return border_radius_style_value(border_bottom_left_radius());
    case PropertyID::BorderBottomRightRadius:
        return border_radius_style_value(border_bottom_right_radius());
    case PropertyID::BorderTopLeftRadius:
        return border_radius_style_value(border_top_left_radius());
    case PropertyID::BorderTopRightRadius:
        return border_radius_style_value(border_top_right_radius());
    case PropertyID::Bottom:
        return length_percentage_or_auto_style_value(inset().bottom());
    case PropertyID::Height:
        return size_style_value(height());
    case PropertyID::Left:
        return length_percentage_or_auto_style_value(inset().left());
    case PropertyID::MarginBottom:
        return length_percentage_or_auto_style_value(margin().bottom());
    case PropertyID::MarginLeft:
        return length_percentage_or_auto_style_value(margin().left());
    case PropertyID::MarginRight:
        return length_percentage_or_auto_style_value(margin().right());
    case PropertyID::MarginTop:
        return length_percentage_or_auto_style_value(margin().top());
    case PropertyID::MaxHeight:
        return size_style_value(max_height());
    case PropertyID::MaxWidth:
        return size_style_value(max_width());
    case PropertyID::MinHeight:
        return size_style_value(min_height());
    case PropertyID::MinWidth:
        return size_style_value(min_width());
    case PropertyID::OutlineColor:
        return color_style_value(outline_color());
    case PropertyID::OutlineStyle:
        return KeywordStyleValue::create(to_keyword(outline_style()));
    case PropertyID::OutlineWidth:
        return length_style_value(outline_width());
    case PropertyID::PaddingBottom:
        return length_percentage_or_auto_style_value(padding().bottom());
    case PropertyID::PaddingLeft:
        return length_percentage_or_auto_style_value(padding().left());
    case PropertyID::PaddingRight:
        return length_percentage_or_auto_style_value(padding().right());
    case PropertyID::PaddingTop:
        return length_percentage_or_auto_style_value(padding().top());
    case PropertyID::ScrollMarginBottom:
        return length_percentage_or_auto_style_value(scroll_margin().bottom());
    case PropertyID::ScrollMarginLeft:
        return length_percentage_or_auto_style_value(scroll_margin().left());
    case PropertyID::ScrollMarginRight:
        return length_percentage_or_auto_style_value(scroll_margin().right());
    case PropertyID::ScrollMarginTop:
        return length_percentage_or_auto_style_value(scroll_margin().top());
    case PropertyID::ScrollPaddingBottom:
        return length_percentage_or_auto_style_value(scroll_padding().bottom());
    case PropertyID::ScrollPaddingLeft:
        return length_percentage_or_auto_style_value(scroll_padding().left());
    case PropertyID::ScrollPaddingRight:
        return length_percentage_or_auto_style_value(scroll_padding().right());
    case PropertyID::ScrollPaddingTop:
        return length_percentage_or_auto_style_value(scroll_padding().top());
    case PropertyID::Right:
        return length_percentage_or_auto_style_value(inset().right());
    case PropertyID::Top:
        return length_percentage_or_auto_style_value(inset().top());
    case PropertyID::Width:
        return size_style_value(width());
    case PropertyID::Display:
        return DisplayStyleValue::create(display());
    case PropertyID::DominantBaseline:
        if (dominant_baseline().has_value())
            return KeywordStyleValue::create(to_keyword(*dominant_baseline()));
        return KeywordStyleValue::create(Keyword::Auto);
    case PropertyID::ColorScheme:
        if (color_schemes().is_empty())
            return ColorSchemeStyleValue::normal();
        return ColorSchemeStyleValue::create(color_schemes(), color_scheme_only());
    case PropertyID::AnimationComposition: {
        StyleValueVector values;
        for (auto composition : animation_compositions())
            values.append(KeywordStyleValue::create(to_keyword(composition)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationDelay: {
        StyleValueVector values;
        for (auto const& delay : animation_delays())
            values.append(TimeStyleValue::create(delay));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationDirection: {
        StyleValueVector values;
        for (auto direction : animation_directions())
            values.append(KeywordStyleValue::create(to_keyword(direction)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationDuration: {
        StyleValueVector values;
        for (auto const& duration : animation_durations()) {
            if (duration.has_value())
                values.append(TimeStyleValue::create(*duration));
            else
                values.append(KeywordStyleValue::create(Keyword::Auto));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationFillMode: {
        StyleValueVector values;
        for (auto fill_mode : animation_fill_modes())
            values.append(KeywordStyleValue::create(to_keyword(fill_mode)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationIterationCount: {
        StyleValueVector values;
        for (auto iteration_count : animation_iteration_counts()) {
            if (isinf(iteration_count))
                values.append(KeywordStyleValue::create(Keyword::Infinite));
            else
                values.append(NumberStyleValue::create(iteration_count));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationName: {
        StyleValueVector values;
        values.ensure_capacity(animation_names().size());
        for (auto const& name : animation_names()) {
            switch (name.syntax) {
            case ComputedAnimationNameSyntax::None:
                values.append(KeywordStyleValue::create(Keyword::None));
                break;
            case ComputedAnimationNameSyntax::CustomIdent:
                values.append(CustomIdentStyleValue::create(name.name));
                break;
            case ComputedAnimationNameSyntax::String:
                values.append(StringStyleValue::create(name.name));
                break;
            }
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationPlayState: {
        StyleValueVector values;
        for (auto play_state : animation_play_states())
            values.append(KeywordStyleValue::create(to_keyword(play_state)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationTimeline: {
        StyleValueVector values;
        for (auto const& timeline : animation_timelines()) {
            switch (timeline.type) {
            case AnimationTimelineData::Type::Auto:
                values.append(KeywordStyleValue::create(Keyword::Auto));
                break;
            case AnimationTimelineData::Type::None:
                values.append(KeywordStyleValue::create(Keyword::None));
                break;
            case AnimationTimelineData::Type::Name:
                values.append(CustomIdentStyleValue::create(timeline.name));
                break;
            case AnimationTimelineData::Type::Scroll: {
                StyleValueTuple arguments;
                arguments.resize_with_default_value(2, nullptr);
                if (timeline.scroller != Scroller::Nearest)
                    arguments[TupleStyleValue::Indices::ScrollFunction::Scroller] = KeywordStyleValue::create(to_keyword(timeline.scroller));
                if (timeline.axis != Axis::Block)
                    arguments[TupleStyleValue::Indices::ScrollFunction::Axis] = KeywordStyleValue::create(to_keyword(timeline.axis));
                values.append(FunctionStyleValue::create("scroll"_utf16_fly_string, TupleStyleValue::create(move(arguments))));
                break;
            }
            case AnimationTimelineData::Type::View: {
                StyleValueTuple arguments;
                arguments.resize_with_default_value(2, nullptr);
                if (timeline.axis != Axis::Block)
                    arguments[TupleStyleValue::Indices::ViewFunction::Axis] = KeywordStyleValue::create(to_keyword(timeline.axis));
                if (!timeline.inset.start.is_auto() || !timeline.inset.end.is_auto()) {
                    arguments[TupleStyleValue::Indices::ViewFunction::Inset] = StyleValueList::create(
                        { length_percentage_or_auto_style_value(timeline.inset.start), length_percentage_or_auto_style_value(timeline.inset.end) },
                        StyleValueList::Separator::Space);
                }
                values.append(FunctionStyleValue::create("view"_utf16_fly_string, TupleStyleValue::create(move(arguments))));
                break;
            }
            }
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::AnimationTimingFunction: {
        if (!animation_timing_function_style_values().is_empty()) {
            auto values = animation_timing_function_style_values();
            return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
        }
        StyleValueVector values;
        for (auto const& timing_function : animation_timing_functions())
            values.append(timing_function.to_style_value());
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::BackdropFilter:
        return filter_style_value(backdrop_filter());
    case PropertyID::FontFamily: {
        StyleValueVector values;
        values.ensure_capacity(font_families().size());
        for (auto const& family : font_families()) {
            family.visit(
                [&](GenericFontFamily generic_family) {
                    values.append(KeywordStyleValue::create(to_keyword(generic_family)));
                },
                [&](ComputedFontFamilyName const& name) {
                    if (name.syntax == ComputedFontFamilySyntax::String)
                        values.append(StringStyleValue::create(name.name));
                    else
                        values.append(CustomIdentStyleValue::create(name.name));
                });
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::FontSize:
        return length_style_value(font_size());
    case PropertyID::FontStyle: {
        ValueComparingRefPtr<StyleValue const> angle;
        if (font_style().angle.has_value()) {
            angle = font_style().angle->visit(
                [](Angle const& angle) -> ValueComparingNonnullRefPtr<StyleValue const> { return AngleStyleValue::create(angle); },
                [](NonnullRefPtr<CalculatedStyleValue const> const& angle) -> ValueComparingNonnullRefPtr<StyleValue const> { return angle; });
        }
        return FontStyleStyleValue::create(font_style().keyword, move(angle));
    }
    case PropertyID::FontWeight:
        return NumberStyleValue::create(font_weight());
    case PropertyID::FontWidth:
        return PercentageStyleValue::create(font_width());
    case PropertyID::FontKerning:
        return KeywordStyleValue::create(to_keyword(font_feature_data().font_kerning));
    case PropertyID::FontFeatureSettings: {
        if (font_feature_data().font_feature_settings.is_empty())
            return KeywordStyleValue::create(Keyword::Normal);
        StyleValueVector settings;
        for (auto const& [tag, value] : font_feature_data().font_feature_settings) {
            settings.append(OpenTypeTaggedStyleValue::create(
                OpenTypeTaggedStyleValue::Mode::FontFeatureSettings,
                tag,
                IntegerStyleValue::create(value)));
        }
        quick_sort(settings, [](auto const& a, auto const& b) {
            return a->as_open_type_tagged().tag() < b->as_open_type_tagged().tag();
        });
        return StyleValueList::create(move(settings), StyleValueList::Separator::Comma);
    }
    case PropertyID::FontLanguageOverride:
        if (font_language_override().has_value())
            return StringStyleValue::create(*font_language_override());
        return KeywordStyleValue::create(Keyword::Normal);
    case PropertyID::FontOpticalSizing:
        return KeywordStyleValue::create(to_keyword(font_optical_sizing()));
    case PropertyID::FontVariantCaps:
        return KeywordStyleValue::create(to_keyword(font_feature_data().font_variant_caps));
    case PropertyID::FontVariantAlternates: {
        if (!font_feature_data().font_variant_alternates.has_value())
            return KeywordStyleValue::create(Keyword::Normal);
        auto const& alternates = *font_feature_data().font_variant_alternates;
        StyleValueVector values;
        if (alternates.historical_forms)
            values.append(KeywordStyleValue::create(Keyword::HistoricalForms));
        auto append_feature_value_function = [&](FontFeatureValueType type, Utf16FlyString name) {
            StyleValueVector names;
            for (auto const& entry : alternates.font_feature_value_entries) {
                if (entry.type == type)
                    names.append(CustomIdentStyleValue::create(entry.name));
            }
            if (!names.is_empty()) {
                values.append(FunctionStyleValue::create(
                    move(name),
                    StyleValueList::create(move(names), StyleValueList::Separator::Space)));
            }
        };
        append_feature_value_function(FontFeatureValueType::Stylistic, "stylistic"_utf16_fly_string);
        append_feature_value_function(FontFeatureValueType::Styleset, "styleset"_utf16_fly_string);
        append_feature_value_function(FontFeatureValueType::CharacterVariant, "character-variant"_utf16_fly_string);
        append_feature_value_function(FontFeatureValueType::Swash, "swash"_utf16_fly_string);
        append_feature_value_function(FontFeatureValueType::Ornaments, "ornaments"_utf16_fly_string);
        append_feature_value_function(FontFeatureValueType::Annotation, "annotation"_utf16_fly_string);
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::FontVariantEastAsian: {
        if (!font_feature_data().font_variant_east_asian.has_value())
            return KeywordStyleValue::create(Keyword::Normal);
        auto const& east_asian = *font_feature_data().font_variant_east_asian;
        StyleValueTuple tuple;
        tuple.resize_with_default_value(3, nullptr);
        if (east_asian.variant.has_value())
            tuple[TupleStyleValue::Indices::FontVariantEastAsian::Variant] = KeywordStyleValue::create(to_keyword(*east_asian.variant));
        if (east_asian.width.has_value())
            tuple[TupleStyleValue::Indices::FontVariantEastAsian::Width] = KeywordStyleValue::create(to_keyword(*east_asian.width));
        if (east_asian.ruby)
            tuple[TupleStyleValue::Indices::FontVariantEastAsian::Ruby] = KeywordStyleValue::create(Keyword::Ruby);
        return TupleStyleValue::create(move(tuple));
    }
    case PropertyID::FontVariantLigatures: {
        if (!font_feature_data().font_variant_ligatures.has_value())
            return KeywordStyleValue::create(Keyword::Normal);
        auto const& ligatures = *font_feature_data().font_variant_ligatures;
        if (ligatures.none)
            return KeywordStyleValue::create(Keyword::None);
        StyleValueTuple tuple;
        tuple.resize_with_default_value(4, nullptr);
        if (ligatures.common.has_value())
            tuple[TupleStyleValue::Indices::FontVariantLigatures::Common] = KeywordStyleValue::create(to_keyword(*ligatures.common));
        if (ligatures.discretionary.has_value())
            tuple[TupleStyleValue::Indices::FontVariantLigatures::Discretionary] = KeywordStyleValue::create(to_keyword(*ligatures.discretionary));
        if (ligatures.historical.has_value())
            tuple[TupleStyleValue::Indices::FontVariantLigatures::Historical] = KeywordStyleValue::create(to_keyword(*ligatures.historical));
        if (ligatures.contextual.has_value())
            tuple[TupleStyleValue::Indices::FontVariantLigatures::Contextual] = KeywordStyleValue::create(to_keyword(*ligatures.contextual));
        return TupleStyleValue::create(move(tuple));
    }
    case PropertyID::FontVariantNumeric: {
        if (!font_feature_data().font_variant_numeric.has_value())
            return KeywordStyleValue::create(Keyword::Normal);
        auto const& numeric = *font_feature_data().font_variant_numeric;
        StyleValueTuple tuple;
        tuple.resize_with_default_value(5, nullptr);
        if (numeric.figure.has_value())
            tuple[TupleStyleValue::Indices::FontVariantNumeric::Figure] = KeywordStyleValue::create(to_keyword(*numeric.figure));
        if (numeric.spacing.has_value())
            tuple[TupleStyleValue::Indices::FontVariantNumeric::Spacing] = KeywordStyleValue::create(to_keyword(*numeric.spacing));
        if (numeric.fraction.has_value())
            tuple[TupleStyleValue::Indices::FontVariantNumeric::Fraction] = KeywordStyleValue::create(to_keyword(*numeric.fraction));
        if (numeric.ordinal)
            tuple[TupleStyleValue::Indices::FontVariantNumeric::Ordinal] = KeywordStyleValue::create(Keyword::Ordinal);
        if (numeric.slashed_zero)
            tuple[TupleStyleValue::Indices::FontVariantNumeric::SlashedZero] = KeywordStyleValue::create(Keyword::SlashedZero);
        return TupleStyleValue::create(move(tuple));
    }
    case PropertyID::FontVariantPosition:
        return KeywordStyleValue::create(to_keyword(font_feature_data().font_variant_position));
    case PropertyID::FontVariationSettings: {
        if (font_variation_settings().is_empty())
            return KeywordStyleValue::create(Keyword::Normal);
        StyleValueVector settings;
        for (auto const& [tag, value] : font_variation_settings()) {
            settings.append(OpenTypeTaggedStyleValue::create(
                OpenTypeTaggedStyleValue::Mode::FontVariationSettings,
                tag,
                NumberStyleValue::create(value)));
        }
        quick_sort(settings, [](auto const& a, auto const& b) {
            return a->as_open_type_tagged().tag() < b->as_open_type_tagged().tag();
        });
        return StyleValueList::create(move(settings), StyleValueList::Separator::Comma);
    }
    case PropertyID::LineHeight:
        return line_height_data().computed_value.visit(
            [](LineHeightData::Normal) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Normal); },
            [](double value) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(value); },
            [](Length const& value) -> NonnullRefPtr<StyleValue const> { return LengthStyleValue::create(value); });
    case PropertyID::LetterSpacing:
        if (letter_spacing_style_value())
            return letter_spacing_style_value();
        return length_style_value(letter_spacing());
    case PropertyID::Opacity:
        return OpacityValueStyleValue::create(NumberStyleValue::create(opacity()));
    case PropertyID::PaintOrder: {
        if (paint_order_is_normal())
            return KeywordStyleValue::create(Keyword::Normal);
        if (paint_order_serialization_length() == 1)
            return KeywordStyleValue::create(to_keyword(paint_order()[0]));
        StyleValueVector values;
        for (u8 i = 0; i < paint_order_serialization_length(); ++i)
            values.append(KeywordStyleValue::create(to_keyword(paint_order()[i])));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::FillOpacity:
        return OpacityValueStyleValue::create(NumberStyleValue::create(fill_opacity()));
    case PropertyID::Fill:
        return svg_paint_style_value(fill());
    case PropertyID::FloodOpacity:
        return OpacityValueStyleValue::create(NumberStyleValue::create(flood_opacity()));
    case PropertyID::StopOpacity:
        return OpacityValueStyleValue::create(NumberStyleValue::create(stop_opacity()));
    case PropertyID::StrokeOpacity:
        return OpacityValueStyleValue::create(NumberStyleValue::create(stroke_opacity()));
    case PropertyID::Stroke:
        return svg_paint_style_value(stroke());
    case PropertyID::BoxSizing:
        return KeywordStyleValue::create(to_keyword(box_sizing()));
    case PropertyID::BoxShadow: {
        if (box_shadow().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector shadows;
        shadows.ensure_capacity(box_shadow().size());
        for (auto const& shadow : box_shadow()) {
            shadows.append(ShadowStyleValue::create(
                ShadowStyleValue::ShadowType::Normal,
                shadow_color_style_value(shadow),
                length_style_value(shadow.offset_x),
                length_style_value(shadow.offset_y),
                length_style_value(shadow.blur_radius),
                length_style_value(shadow.spread_distance),
                shadow.placement));
        }
        return StyleValueList::create(move(shadows), StyleValueList::Separator::Comma);
    }
    case PropertyID::Clear:
        return KeywordStyleValue::create(to_keyword(clear()));
    case PropertyID::ClipPath:
        if (!clip_path().has_value())
            return KeywordStyleValue::create(Keyword::None);
        if (clip_path()->is_url())
            return URLStyleValue::create(clip_path()->url());
        return NonnullRefPtr { clip_path()->basic_shape() };
    case PropertyID::Clip: {
        if (clip().is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        auto rect = clip().to_rect();
        return RectStyleValue::create(
            length_or_auto_style_value(rect.top_edge),
            length_or_auto_style_value(rect.right_edge),
            length_or_auto_style_value(rect.bottom_edge),
            length_or_auto_style_value(rect.left_edge));
    }
    case PropertyID::ColumnCount:
        if (column_count().is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        return IntegerStyleValue::create(column_count().value());
    case PropertyID::ColumnGap:
        return column_gap().visit(
            [&](LengthPercentage const& gap) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(gap); },
            [](NormalGap) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Normal); });
    case PropertyID::Contain: {
        if (contain().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector containment_types;
        if (contain().size_containment)
            containment_types.append(KeywordStyleValue::create(Keyword::Size));
        if (contain().inline_size_containment)
            containment_types.append(KeywordStyleValue::create(Keyword::InlineSize));
        if (contain().layout_containment)
            containment_types.append(KeywordStyleValue::create(Keyword::Layout));
        if (contain().style_containment)
            containment_types.append(KeywordStyleValue::create(Keyword::Style));
        if (contain().paint_containment)
            containment_types.append(KeywordStyleValue::create(Keyword::Paint));
        return StyleValueList::create(move(containment_types), StyleValueList::Separator::Space);
    }
    case PropertyID::ContainerName: {
        if (container_name().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector names;
        names.ensure_capacity(container_name().size());
        for (auto const& name : container_name())
            names.append(CustomIdentStyleValue::create(name));
        return StyleValueList::create(move(names), StyleValueList::Separator::Space);
    }
    case PropertyID::ContainerType: {
        if (container_type().is_empty())
            return KeywordStyleValue::create(Keyword::Normal);
        StyleValueVector types;
        if (container_type().is_size_container)
            types.append(KeywordStyleValue::create(Keyword::Size));
        if (container_type().is_inline_size_container)
            types.append(KeywordStyleValue::create(Keyword::InlineSize));
        if (container_type().is_scroll_state_container)
            types.append(KeywordStyleValue::create(Keyword::ScrollState));
        return StyleValueList::create(move(types), StyleValueList::Separator::Space);
    }
    case PropertyID::Content:
        switch (computed_content().type) {
        case ComputedContentData::Type::Normal:
            return KeywordStyleValue::create(Keyword::Normal);
        case ComputedContentData::Type::None:
            return KeywordStyleValue::create(Keyword::None);
        case ComputedContentData::Type::List: {
            StyleValueVector items;
            for (auto const& item : computed_content().items)
                items.append(computed_content_item_style_value(item));
            StyleValueVector alt_text;
            for (auto const& item : computed_content().alt_text)
                alt_text.append(computed_content_item_style_value(item));
            ValueComparingRefPtr<StyleValueList const> alt_text_style_value;
            if (!alt_text.is_empty())
                alt_text_style_value = StyleValueList::create(move(alt_text), StyleValueList::Separator::Space);
            return ContentStyleValue::create(
                StyleValueList::create(move(items), StyleValueList::Separator::Space),
                move(alt_text_style_value));
        }
        }
        VERIFY_NOT_REACHED();
    case PropertyID::ContentVisibility:
        return KeywordStyleValue::create(to_keyword(content_visibility()));
    case PropertyID::Cursor: {
        if (cursor().size() == 1 && cursor().first().has<CursorPredefined>())
            return KeywordStyleValue::create(to_keyword(cursor().first().get<CursorPredefined>()));
        StyleValueVector cursors;
        cursors.ensure_capacity(cursor().size());
        for (auto const& cursor : cursor()) {
            cursor.visit(
                [&](NonnullRefPtr<CursorStyleValue const> const& cursor) { cursors.append(cursor); },
                [&](CursorPredefined cursor) { cursors.append(KeywordStyleValue::create(to_keyword(cursor))); });
        }
        return StyleValueList::create(move(cursors), StyleValueList::Separator::Comma);
    }
    case PropertyID::CounterIncrement:
    case PropertyID::CounterReset:
    case PropertyID::CounterSet: {
        auto const* counters = &counter_increment();
        if (property_id == PropertyID::CounterReset)
            counters = &counter_reset();
        else if (property_id == PropertyID::CounterSet)
            counters = &counter_set();
        if (counters->is_empty())
            return KeywordStyleValue::create(Keyword::None);
        Vector<CounterDefinition> definitions;
        definitions.ensure_capacity(counters->size());
        for (auto const& counter : *counters) {
            ValueComparingRefPtr<StyleValue const> value;
            if (counter.value.has_value())
                value = IntegerStyleValue::create(*counter.value);
            definitions.append(CounterDefinition {
                .name = counter.name,
                .is_reversed = counter.is_reversed,
                .value = move(value),
            });
        }
        return CounterDefinitionsStyleValue::create(move(definitions));
    }
    case PropertyID::CornerBottomLeftShape:
        return SuperellipseStyleValue::create(NumberStyleValue::create(corner_bottom_left_shape()));
    case PropertyID::CornerBottomRightShape:
        return SuperellipseStyleValue::create(NumberStyleValue::create(corner_bottom_right_shape()));
    case PropertyID::CornerTopLeftShape:
        return SuperellipseStyleValue::create(NumberStyleValue::create(corner_top_left_shape()));
    case PropertyID::CornerTopRightShape:
        return SuperellipseStyleValue::create(NumberStyleValue::create(corner_top_right_shape()));
    case PropertyID::Cx:
        return length_percentage_style_value(cx());
    case PropertyID::Cy:
        return length_percentage_style_value(cy());
    case PropertyID::Direction:
        return KeywordStyleValue::create(to_keyword(direction()));
    case PropertyID::EmptyCells:
        return KeywordStyleValue::create(to_keyword(empty_cells()));
    case PropertyID::FillRule:
        return KeywordStyleValue::create(to_keyword(fill_rule()));
    case PropertyID::Float:
        return KeywordStyleValue::create(to_keyword(float_()));
    case PropertyID::FlexDirection:
        return KeywordStyleValue::create(to_keyword(flex_direction()));
    case PropertyID::FlexBasis:
        return flex_basis().visit(
            [](FlexBasisContent) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Content); },
            [&](Size const& size) -> NonnullRefPtr<StyleValue const> { return size_style_value(size); });
    case PropertyID::FlexGrow:
        return NumberStyleValue::create(flex_grow());
    case PropertyID::FlexShrink:
        return NumberStyleValue::create(flex_shrink());
    case PropertyID::FlexWrap:
        return KeywordStyleValue::create(to_keyword(flex_wrap()));
    case PropertyID::Filter:
        return filter_style_value(filter());
    case PropertyID::GridAutoColumns:
        return GridTrackSizeListStyleValue::create(grid_auto_columns());
    case PropertyID::GridAutoFlow:
        return GridAutoFlowStyleValue::create(
            grid_auto_flow().row ? GridAutoFlowStyleValue::Axis::Row : GridAutoFlowStyleValue::Axis::Column,
            grid_auto_flow().dense ? GridAutoFlowStyleValue::Dense::Yes : GridAutoFlowStyleValue::Dense::No);
    case PropertyID::GridAutoRows:
        return GridTrackSizeListStyleValue::create(grid_auto_rows());
    case PropertyID::GridColumnEnd:
        return GridTrackPlacementStyleValue::create(grid_column_end());
    case PropertyID::GridColumnStart:
        return GridTrackPlacementStyleValue::create(grid_column_start());
    case PropertyID::GridRowEnd:
        return GridTrackPlacementStyleValue::create(grid_row_end());
    case PropertyID::GridRowStart:
        return GridTrackPlacementStyleValue::create(grid_row_start());
    case PropertyID::GridTemplateAreas:
        return GridTemplateAreaStyleValue::create(grid_template_areas().areas, grid_template_areas().row_count, grid_template_areas().column_count);
    case PropertyID::GridTemplateColumns:
        return GridTrackSizeListStyleValue::create(grid_template_columns());
    case PropertyID::GridTemplateRows:
        return GridTrackSizeListStyleValue::create(grid_template_rows());
    case PropertyID::FontVariantEmoji:
        return KeywordStyleValue::create(to_keyword(font_variant_emoji()));
    case PropertyID::ImageRendering:
        return KeywordStyleValue::create(to_keyword(image_rendering()));
    case PropertyID::Isolation:
        return KeywordStyleValue::create(to_keyword(isolation()));
    case PropertyID::JustifyContent:
        return KeywordStyleValue::create(to_keyword(justify_content()));
    case PropertyID::JustifyItems:
        return KeywordStyleValue::create(to_keyword(justify_items()));
    case PropertyID::JustifySelf:
        return KeywordStyleValue::create(to_keyword(justify_self()));
    case PropertyID::ListStylePosition:
        return KeywordStyleValue::create(to_keyword(list_style_position()));
    case PropertyID::ListStyleType:
        return list_style_type().visit(
            [](Empty) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::None); },
            [](RefPtr<CounterStyle const> const& counter_style) -> NonnullRefPtr<StyleValue const> {
                VERIFY(counter_style);
                return CounterStyleStyleValue::create(counter_style->name());
            },
            [](Utf16String const& string) -> NonnullRefPtr<StyleValue const> { return StringStyleValue::create(string); },
            [](Utf16FlyString const& name) -> NonnullRefPtr<StyleValue const> { return CounterStyleStyleValue::create(name); },
            [](ListStyleSymbols const& symbols) -> NonnullRefPtr<StyleValue const> {
                return CounterStyleStyleValue::create(CounterStyleStyleValue::SymbolsFunction {
                    .type = symbols.type,
                    .symbols = symbols.symbols,
                });
            });
    case PropertyID::ListStyleImage:
        if (list_style_image())
            return *list_style_image();
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::MaskClip: {
        StyleValueVector values;
        for (auto const& layer : mask_layers()) {
            if (layer.mask_clip_is_no_clip)
                values.append(KeywordStyleValue::create(Keyword::NoClip));
            else
                values.append(KeywordStyleValue::create(to_keyword(layer.mask_clip)));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskComposite: {
        StyleValueVector values;
        for (auto const& layer : mask_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.mask_composite)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskImage: {
        StyleValueVector values;
        for (auto const& layer : mask_layers()) {
            if (layer.image_style_value)
                values.append(*layer.image_style_value);
            else if (layer.background_image)
                values.append(*layer.background_image);
            else
                values.append(KeywordStyleValue::create(Keyword::None));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskMode: {
        StyleValueVector values;
        for (auto const& layer : mask_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.mask_mode)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskOrigin: {
        StyleValueVector values;
        for (auto const& layer : mask_layers())
            values.append(KeywordStyleValue::create(to_keyword(layer.mask_origin)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskPosition: {
        StyleValueVector values;
        for (auto const& position : mask_positions())
            values.append(position_style_value(position));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskRepeat: {
        StyleValueVector values;
        for (auto const& layer : mask_layers())
            values.append(RepeatStyleStyleValue::create(layer.repeat_x, layer.repeat_y));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskSize: {
        StyleValueVector values;
        for (auto const& layer : mask_layers()) {
            switch (layer.size_type) {
            case CSS::BackgroundSize::Contain:
                values.append(KeywordStyleValue::create(Keyword::Contain));
                break;
            case CSS::BackgroundSize::Cover:
                values.append(KeywordStyleValue::create(Keyword::Cover));
                break;
            case CSS::BackgroundSize::LengthPercentage:
                values.append(BackgroundSizeStyleValue::create(
                    length_percentage_or_auto_style_value(layer.size_x),
                    length_percentage_or_auto_style_value(layer.size_y)));
                break;
            }
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::MaskType:
        return KeywordStyleValue::create(to_keyword(mask_type()));
    case PropertyID::MathDepth:
        return IntegerStyleValue::create(math_depth());
    case PropertyID::MathShift:
        return KeywordStyleValue::create(to_keyword(math_shift()));
    case PropertyID::MathStyle:
        return KeywordStyleValue::create(to_keyword(math_style()));
    case PropertyID::MixBlendMode:
        return KeywordStyleValue::create(to_keyword(mix_blend_mode()));
    case PropertyID::ObjectFit:
        return KeywordStyleValue::create(to_keyword(object_fit()));
    case PropertyID::ObjectPosition:
        return position_style_value(object_position());
    case PropertyID::Order:
        return IntegerStyleValue::create(order());
    case PropertyID::OutlineOffset:
        if (outline_offset_style_value())
            return outline_offset_style_value();
        return length_style_value(outline_offset());
    case PropertyID::Orphans:
        return IntegerStyleValue::create(orphans());
    case PropertyID::OverflowWrap:
        switch (overflow_wrap()) {
        case OverflowWrap::Normal:
            return KeywordStyleValue::create(Keyword::Normal);
        case OverflowWrap::BreakWord:
            return KeywordStyleValue::create(Keyword::BreakWord);
        case OverflowWrap::Anywhere:
            return KeywordStyleValue::create(Keyword::Anywhere);
        }
        VERIFY_NOT_REACHED();
    case PropertyID::OverflowX:
        return KeywordStyleValue::create(to_keyword(overflow_x()));
    case PropertyID::OverflowY:
        return KeywordStyleValue::create(to_keyword(overflow_y()));
    case PropertyID::OverflowClipMarginBottom:
    case PropertyID::OverflowClipMarginLeft:
    case PropertyID::OverflowClipMarginRight:
    case PropertyID::OverflowClipMarginTop: {
        auto const* side = &overflow_clip_margin().top;
        if (property_id == PropertyID::OverflowClipMarginRight)
            side = &overflow_clip_margin().right;
        else if (property_id == PropertyID::OverflowClipMarginBottom)
            side = &overflow_clip_margin().bottom;
        else if (property_id == PropertyID::OverflowClipMarginLeft)
            side = &overflow_clip_margin().left;
        return OverflowClipMarginStyleValue::create(side->visual_box, length_style_value(side->offset));
    }
    case PropertyID::PointerEvents:
        return KeywordStyleValue::create(to_keyword(pointer_events()));
    case PropertyID::Position:
        return KeywordStyleValue::create(to_keyword(position()));
    case PropertyID::PositionAnchor:
        switch (position_anchor_value().type) {
        case PositionAnchor::Type::Normal:
            return KeywordStyleValue::create(Keyword::Normal);
        case PositionAnchor::Type::None:
            return KeywordStyleValue::create(Keyword::None);
        case PositionAnchor::Type::Auto:
            return KeywordStyleValue::create(Keyword::Auto);
        case PositionAnchor::Type::Name:
            VERIFY(position_anchor_value().name.has_value());
            return CustomIdentStyleValue::create(*position_anchor_value().name);
        }
        VERIFY_NOT_REACHED();
    case PropertyID::PositionArea:
        if (position_area().keywords.is_empty())
            return KeywordStyleValue::create(Keyword::None);
        return position_area_style_value(position_area());
    case PropertyID::PositionTryFallbacks: {
        if (position_try_fallbacks().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector fallbacks;
        for (auto const& fallback : position_try_fallbacks()) {
            if (fallback.position_area.has_value()) {
                fallbacks.append(position_area_style_value(*fallback.position_area));
                continue;
            }
            StyleValueVector values;
            if (fallback.name.has_value())
                values.append(CustomIdentStyleValue::create(*fallback.name));
            for (auto tactic : fallback.tactics)
                values.append(KeywordStyleValue::create(to_keyword(tactic)));
            fallbacks.append(StyleValueList::create(move(values), StyleValueList::Separator::Space));
        }
        return StyleValueList::create(move(fallbacks), StyleValueList::Separator::Comma);
    }
    case PropertyID::PositionTryOrder:
        if (!position_try_order().has_value())
            return KeywordStyleValue::create(Keyword::Normal);
        return KeywordStyleValue::create(to_keyword(*position_try_order()));
    case PropertyID::PositionVisibility: {
        if (position_visibility().always)
            return KeywordStyleValue::create(Keyword::Always);
        StyleValueVector values;
        if (position_visibility().anchors_valid)
            values.append(KeywordStyleValue::create(Keyword::AnchorsValid));
        if (position_visibility().anchors_visible)
            values.append(KeywordStyleValue::create(Keyword::AnchorsVisible));
        if (position_visibility().no_overflow)
            values.append(KeywordStyleValue::create(Keyword::NoOverflow));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::Perspective:
        if (perspective().has_value())
            return length_style_value(*perspective());
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::PerspectiveOrigin:
        return position_style_value(perspective_origin());
    case PropertyID::Resize:
        return KeywordStyleValue::create(to_keyword(resize()));
    case PropertyID::RowGap:
        return row_gap().visit(
            [&](LengthPercentage const& gap) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(gap); },
            [](NormalGap) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Normal); });
    case PropertyID::Quotes:
        switch (quotes().type) {
        case QuotesData::Type::None:
            return KeywordStyleValue::create(Keyword::None);
        case QuotesData::Type::Auto:
            return KeywordStyleValue::create(Keyword::Auto);
        case QuotesData::Type::Specified: {
            StyleValueVector strings;
            strings.ensure_capacity(quotes().strings.size() * 2);
            for (auto const& pair : quotes().strings) {
                strings.append(StringStyleValue::create(pair[0]));
                strings.append(StringStyleValue::create(pair[1]));
            }
            return StyleValueList::create(move(strings), StyleValueList::Separator::Space);
        }
        }
        VERIFY_NOT_REACHED();
    case PropertyID::R:
        return length_percentage_style_value(r());
    case PropertyID::Rx:
        return length_percentage_or_auto_style_value(rx());
    case PropertyID::Ry:
        return length_percentage_or_auto_style_value(ry());
    case PropertyID::Rotate:
        if (rotate())
            return *rotate();
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::Scale:
        if (scale())
            return *scale();
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::ScrollTimelineAxis:
        return timeline_axis_list_style_value(scroll_timeline_axes());
    case PropertyID::ScrollTimelineName:
        return timeline_name_list_style_value(scroll_timeline_names());
    case PropertyID::ScrollBehavior:
        return KeywordStyleValue::create(to_keyword(scroll_behavior()));
    case PropertyID::ScrollbarColor:
        if (scrollbar_color().is_auto)
            return KeywordStyleValue::create(Keyword::Auto);
        return ScrollbarColorStyleValue::create(
            color_style_value(scrollbar_color().thumb_color),
            color_style_value(scrollbar_color().track_color));
    case PropertyID::ScrollbarGutter:
        return ScrollbarGutterStyleValue::create(scrollbar_gutter());
    case PropertyID::ScrollbarWidth:
        return KeywordStyleValue::create(to_keyword(scrollbar_width()));
    case PropertyID::ShapeImageThreshold:
        return OpacityValueStyleValue::create(NumberStyleValue::create(shape_image_threshold()));
    case PropertyID::ShapeMargin:
        return length_percentage_style_value(shape_margin());
    case PropertyID::ShapeOutside: {
        if (shape_outside().image.has<URL>())
            return URLStyleValue::create(shape_outside().image.get<URL>());
        if (shape_outside().image.has<NonnullRefPtr<AbstractImageStyleValue const>>())
            return shape_outside().image.get<NonnullRefPtr<AbstractImageStyleValue const>>();
        if (!shape_outside().basic_shape && !shape_outside().shape_box.has_value())
            return KeywordStyleValue::create(Keyword::None);
        if (!shape_outside().shape_box.has_value())
            return *shape_outside().basic_shape;
        auto shape_box = KeywordStyleValue::create(to_keyword(*shape_outside().shape_box));
        if (!shape_outside().basic_shape)
            return shape_box;
        return StyleValueList::create({ *shape_outside().basic_shape, move(shape_box) }, StyleValueList::Separator::Space);
    }
    case PropertyID::ShapeRendering:
        return KeywordStyleValue::create(to_keyword(shape_rendering()));
    case PropertyID::StrokeLinecap:
        return KeywordStyleValue::create(to_keyword(stroke_linecap()));
    case PropertyID::StrokeLinejoin:
        return KeywordStyleValue::create(to_keyword(stroke_linejoin()));
    case PropertyID::StrokeDasharray: {
        if (stroke_dasharray().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector values;
        values.ensure_capacity(stroke_dasharray().size());
        for (auto const& value : stroke_dasharray()) {
            values.append(value.visit(
                [&](LengthPercentage const& length_percentage) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(length_percentage); },
                [](float number) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(number); }));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::StrokeDashoffset:
        return length_percentage_style_value(stroke_dashoffset());
    case PropertyID::StrokeMiterlimit:
        return NumberStyleValue::create(stroke_miterlimit());
    case PropertyID::StrokeWidth:
        return length_percentage_style_value(stroke_width());
    case PropertyID::TableLayout:
        return KeywordStyleValue::create(to_keyword(table_layout()));
    case PropertyID::TabSize:
        return tab_size().visit(
            [&](CSSPixels value) -> NonnullRefPtr<StyleValue const> { return length_style_value(value); },
            [](double value) -> NonnullRefPtr<StyleValue const> { return NumberStyleValue::create(value); });
    case PropertyID::TextDecorationLine: {
        if (text_decoration_line().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector values;
        values.ensure_capacity(text_decoration_line().size());
        for (auto line : text_decoration_line())
            values.append(KeywordStyleValue::create(to_keyword(line)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::TextIndent:
        return TextIndentStyleValue::create(
            length_percentage_style_value(text_indent().length_percentage),
            text_indent().hanging ? TextIndentStyleValue::Hanging::Yes : TextIndentStyleValue::Hanging::No,
            text_indent().each_line ? TextIndentStyleValue::EachLine::Yes : TextIndentStyleValue::EachLine::No);
    case PropertyID::TextAlign:
        return KeywordStyleValue::create(to_keyword(text_align()));
    case PropertyID::TextAnchor:
        return KeywordStyleValue::create(to_keyword(text_anchor()));
    case PropertyID::TextDecorationSkipInk:
        return KeywordStyleValue::create(to_keyword(text_decoration_skip_ink()));
    case PropertyID::TextDecorationStyle:
        return KeywordStyleValue::create(to_keyword(text_decoration_style()));
    case PropertyID::TextDecorationThickness:
        return text_decoration_thickness().value.visit(
            [](TextDecorationThickness::Auto) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Auto); },
            [](TextDecorationThickness::FromFont) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::FromFont); },
            [&](LengthPercentage const& thickness) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(thickness); });
    case PropertyID::TextJustify:
        return KeywordStyleValue::create(to_keyword(text_justify()));
    case PropertyID::TextOverflow:
        return KeywordStyleValue::create(to_keyword(text_overflow()));
    case PropertyID::TextTransform:
        return KeywordStyleValue::create(to_keyword(text_transform()));
    case PropertyID::TextUnderlineOffset:
        return text_underline_offset_value().computed_value.visit(
            [](TextUnderlineOffset::Auto) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(Keyword::Auto); },
            [&](LengthPercentage const& offset) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(offset); });
    case PropertyID::TextWrapStyle:
        return KeywordStyleValue::create(to_keyword(text_wrap_style()));
    case PropertyID::TimelineScope:
        if (timeline_scope().all)
            return KeywordStyleValue::create(Keyword::All);
        if (timeline_scope().names.is_empty())
            return KeywordStyleValue::create(Keyword::None);
        return custom_ident_list_style_value(timeline_scope().names);
    case PropertyID::TextShadow: {
        if (text_shadow().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector shadows;
        shadows.ensure_capacity(text_shadow().size());
        for (auto const& shadow : text_shadow()) {
            shadows.append(ShadowStyleValue::create(
                ShadowStyleValue::ShadowType::Text,
                shadow_color_style_value(shadow),
                length_style_value(shadow.offset_x),
                length_style_value(shadow.offset_y),
                length_style_value(shadow.blur_radius),
                {},
                ShadowPlacement::Outer));
        }
        return StyleValueList::create(move(shadows), StyleValueList::Separator::Comma);
    }
    case PropertyID::TextRendering:
        return KeywordStyleValue::create(to_keyword(font_feature_data().text_rendering));
    case PropertyID::TextUnderlinePosition:
        return TextUnderlinePositionStyleValue::create(text_underline_position().horizontal, text_underline_position().vertical);
    case PropertyID::TextWrapMode:
        return KeywordStyleValue::create(to_keyword(text_wrap_mode()));
    case PropertyID::TransformBox:
        return KeywordStyleValue::create(to_keyword(transform_box()));
    case PropertyID::Transform: {
        if (transformations().is_empty())
            return KeywordStyleValue::create(Keyword::None);
        StyleValueVector values;
        values.ensure_capacity(transformations().size());
        for (auto const& transformation : transformations())
            values.append(transformation);
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::TransformOrigin:
        return StyleValueList::create(
            { length_percentage_style_value(transform_origin().x), length_percentage_style_value(transform_origin().y), length_percentage_style_value(transform_origin().z) },
            StyleValueList::Separator::Space);
    case PropertyID::TransformStyle:
        return KeywordStyleValue::create(to_keyword(transform_style()));
    case PropertyID::TransitionBehavior: {
        StyleValueVector values;
        for (auto behavior : transition_behaviors())
            values.append(KeywordStyleValue::create(to_keyword(behavior)));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::TransitionDelay: {
        StyleValueVector values;
        for (auto const& delay : transition_delays())
            values.append(TimeStyleValue::create(delay));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::TransitionDuration: {
        StyleValueVector values;
        for (auto const& duration : transition_durations())
            values.append(TimeStyleValue::create(duration));
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::TransitionProperty: {
        StyleValueVector values;
        for (auto const& property : transition_properties()) {
            if (property.has_value())
                values.append(CustomIdentStyleValue::create(*property));
            else
                values.append(KeywordStyleValue::create(Keyword::None));
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::TransitionTimingFunction: {
        if (!transition_timing_function_style_values().is_empty()) {
            auto values = transition_timing_function_style_values();
            return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
        }
        StyleValueVector values;
        for (auto const& timing_function : transition_timing_functions())
            values.append(timing_function.to_style_value());
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::TouchAction: {
        auto const action = touch_action();
        if (action.allow_left && action.allow_right && action.allow_up && action.allow_down && action.allow_pinch_zoom) {
            if (action.allow_other)
                return KeywordStyleValue::create(Keyword::Auto);
            return KeywordStyleValue::create(Keyword::Manipulation);
        }
        if (!action.allow_left && !action.allow_right && !action.allow_up && !action.allow_down && !action.allow_pinch_zoom)
            return KeywordStyleValue::create(Keyword::None);

        StyleValueVector values;
        if (action.allow_left && action.allow_right)
            values.append(KeywordStyleValue::create(Keyword::PanX));
        else if (action.allow_left)
            values.append(KeywordStyleValue::create(Keyword::PanLeft));
        else if (action.allow_right)
            values.append(KeywordStyleValue::create(Keyword::PanRight));
        if (action.allow_up && action.allow_down)
            values.append(KeywordStyleValue::create(Keyword::PanY));
        else if (action.allow_up)
            values.append(KeywordStyleValue::create(Keyword::PanUp));
        else if (action.allow_down)
            values.append(KeywordStyleValue::create(Keyword::PanDown));
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::Translate:
        if (translate())
            return *translate();
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::UnicodeBidi:
        return KeywordStyleValue::create(to_keyword(unicode_bidi()));
    case PropertyID::UserSelect:
        return KeywordStyleValue::create(to_keyword(user_select()));
    case PropertyID::VectorEffect:
        return KeywordStyleValue::create(to_keyword(vector_effect()));
    case PropertyID::ViewTimelineAxis:
        return timeline_axis_list_style_value(view_timeline_axes());
    case PropertyID::ViewTimelineInset: {
        StyleValueVector insets;
        for (auto const& inset : view_timeline_insets()) {
            insets.append(StyleValueList::create(
                { length_percentage_or_auto_style_value(inset.start), length_percentage_or_auto_style_value(inset.end) },
                StyleValueList::Separator::Space));
        }
        return StyleValueList::create(move(insets), StyleValueList::Separator::Comma);
    }
    case PropertyID::ViewTimelineName:
        return timeline_name_list_style_value(view_timeline_names());
    case PropertyID::VerticalAlign:
        return vertical_align().visit(
            [](VerticalAlign alignment) -> NonnullRefPtr<StyleValue const> { return KeywordStyleValue::create(to_keyword(alignment)); },
            [&](LengthPercentage const& offset) -> NonnullRefPtr<StyleValue const> { return length_percentage_style_value(offset); });
    case PropertyID::ViewTransitionName:
        if (view_transition_name().has_value())
            return CustomIdentStyleValue::create(*view_transition_name());
        return KeywordStyleValue::create(Keyword::None);
    case PropertyID::Visibility:
        return KeywordStyleValue::create(to_keyword(visibility()));
    case PropertyID::Widows:
        return IntegerStyleValue::create(widows());
    case PropertyID::WhiteSpaceCollapse:
        return KeywordStyleValue::create(to_keyword(white_space_collapse()));
    case PropertyID::WhiteSpaceTrim: {
        StyleValueVector values;
        if (white_space_trim().discard_before)
            values.append(KeywordStyleValue::create(Keyword::DiscardBefore));
        if (white_space_trim().discard_after)
            values.append(KeywordStyleValue::create(Keyword::DiscardAfter));
        if (white_space_trim().discard_inner)
            values.append(KeywordStyleValue::create(Keyword::DiscardInner));
        if (values.is_empty())
            return KeywordStyleValue::create(Keyword::None);
        return StyleValueList::create(move(values), StyleValueList::Separator::Space);
    }
    case PropertyID::WillChange: {
        if (will_change().is_auto())
            return KeywordStyleValue::create(Keyword::Auto);
        StyleValueVector values;
        values.ensure_capacity(will_change().entries().size());
        for (auto const& entry : will_change().entries()) {
            entry.visit(
                [&](WillChange::Type type) {
                    values.append(KeywordStyleValue::create(type == WillChange::Type::Contents ? Keyword::Contents : Keyword::ScrollPosition));
                },
                [&](PropertyID property_id) {
                    values.append(CustomIdentStyleValue::create(string_from_property_id(property_id)));
                });
        }
        return StyleValueList::create(move(values), StyleValueList::Separator::Comma);
    }
    case PropertyID::WordBreak:
        return KeywordStyleValue::create(to_keyword(word_break()));
    case PropertyID::WritingMode:
        return KeywordStyleValue::create(to_keyword(writing_mode()));
    case PropertyID::WordSpacing:
        if (word_spacing_style_value())
            return word_spacing_style_value();
        return length_style_value(word_spacing());
    case PropertyID::X:
        return length_percentage_style_value(x());
    case PropertyID::Y:
        return length_percentage_style_value(y());
    case PropertyID::ZIndex:
        if (z_index().has_value())
            return IntegerStyleValue::create(*z_index());
        return KeywordStyleValue::create(Keyword::Auto);
    default:
        return {};
    }
}

RefPtr<StyleValue const> ComputedValues::computed_style_value_for_inheritance(PropertyID property_id, WithAnimationsApplied with_animations_applied) const
{
    if (with_animations_applied == WithAnimationsApplied::No && m_base_values)
        return m_base_values->computed_style_value_for_inheritance(property_id);

    if (auto value = m_inheritance_dependent_specified_values.get(property_id); value.has_value() && value.value()->depends_on_current_color())
        return *value;

    return computed_style_value(property_id, with_animations_applied);
}

static size_t property_bitmap_index(PropertyID property_id)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    return to_underlying(property_id) - to_underlying(first_longhand_property_id);
}

ComputedProperties::Builder::Builder()
    : m_data(adopt_ref(*new Data))
    , m_style(adopt_ref(*new ComputedProperties(m_data, false, false)))
{
}

ComputedProperties::Builder::Builder(ComputedProperties const& style)
    : Builder()
{
    m_data->property_values = style.data().property_values;
    m_data->property_important = style.data().property_important;
    m_data->property_inherited = style.data().property_inherited;
    m_data->display_before_box_type_transformation = style.data().display_before_box_type_transformation;
    m_data->pseudo_element_styles = style.data().pseudo_element_styles;
    m_data->line_height = style.data().line_height;
    m_data->inheritance_dependent_specified_values = style.data().inheritance_dependent_specified_values;
    m_data->raw_cascaded_font_size = style.data().raw_cascaded_font_size;
    m_depends_on_viewport_metrics = style.depends_on_viewport_metrics();
    m_font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics();
    m_in_display_none_subtree = style.in_display_none_subtree();
    m_style->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    m_style->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    m_style->m_in_display_none_subtree = m_in_display_none_subtree;
    if (style.m_animated_properties)
        m_style->m_animated_properties = adopt_ref(*new AnimatedProperties(*style.m_animated_properties));
}

NonnullRefPtr<ComputedProperties> ComputedProperties::Builder::build() &&
{
    m_style->m_depends_on_viewport_metrics = m_depends_on_viewport_metrics;
    m_style->m_font_metrics_depend_on_viewport_metrics = m_font_metrics_depend_on_viewport_metrics;
    m_style->m_in_display_none_subtree = m_in_display_none_subtree;
    return move(m_style);
}

ComputedProperties::Builder ComputedProperties::create_builder()
{
    return Builder {};
}

ComputedProperties::Builder ComputedProperties::create_builder_with_base_values_from(ComputedProperties const& style)
{
    return Builder { style };
}

NonnullRefPtr<ComputedProperties> ComputedProperties::create(Builder&& builder)
{
    return move(builder).build();
}

AnimatedProperties::AnimatedProperties(AnimatedProperties const& other)
    : m_has_property(other.m_has_property)
    , m_property_inherited(other.m_property_inherited)
    , m_property_result_of_transition(other.m_property_result_of_transition)
    , m_values(other.m_values)
{
}

ComputedProperties::ComputedProperties(NonnullRefPtr<Data const> data, bool depends_on_viewport_metrics, bool font_metrics_depend_on_viewport_metrics)
    : m_data(move(data))
    , m_depends_on_viewport_metrics(depends_on_viewport_metrics)
    , m_font_metrics_depend_on_viewport_metrics(font_metrics_depend_on_viewport_metrics)
{
}

ComputedProperties::~ComputedProperties() = default;

NonnullRefPtr<ComputedProperties> ComputedProperties::copy_without_animations() const
{
    auto copy = adopt_ref(*new ComputedProperties(m_data, m_depends_on_viewport_metrics, m_font_metrics_depend_on_viewport_metrics));
    copy->m_in_display_none_subtree = m_in_display_none_subtree;
    return copy;
}

AnimatedProperties const& ComputedProperties::animated_properties() const
{
    static NeverDestroyed<AnimatedProperties> empty_animated_properties;
    if (!m_animated_properties)
        return *empty_animated_properties;
    return *m_animated_properties;
}

AnimatedProperties& ComputedProperties::mutable_animated_properties()
{
    if (!m_animated_properties)
        m_animated_properties = adopt_ref(*new AnimatedProperties);
    if (m_animated_properties->ref_count() > 1)
        m_animated_properties = adopt_ref(*new AnimatedProperties(*m_animated_properties));
    return *m_animated_properties;
}

bool AnimatedProperties::has_property(PropertyID property_id) const
{
    return m_has_property.get(property_bitmap_index(property_id));
}

bool AnimatedProperties::is_property_inherited(PropertyID property_id) const
{
    return m_property_inherited.get(property_bitmap_index(property_id));
}

bool AnimatedProperties::is_property_result_of_transition(PropertyID property_id) const
{
    return m_property_result_of_transition.get(property_bitmap_index(property_id));
}

StyleValue const& AnimatedProperties::property(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);
    VERIFY(has_property(property_id));

    auto animated_value = m_values.get(property_id);
    VERIFY(animated_value.has_value());
    return *animated_value.value();
}

void AnimatedProperties::set_property_inherited(PropertyID property_id, ComputedProperties::Inherited inherited)
{
    m_property_inherited.set(property_bitmap_index(property_id), inherited == ComputedProperties::Inherited::Yes);
}

void AnimatedProperties::set_property_result_of_transition(PropertyID property_id, AnimatedPropertyResultOfTransition animated_value_result_of_transition)
{
    m_property_result_of_transition.set(property_bitmap_index(property_id), animated_value_result_of_transition == AnimatedPropertyResultOfTransition::Yes);
}

void AnimatedProperties::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, ComputedProperties::Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_values.set(id, move(value));

    m_has_property.set(property_bitmap_index(id), true);

    set_property_inherited(id, inherited);
    set_property_result_of_transition(id, animated_property_result_of_transition);
}

void AnimatedProperties::remove_property(PropertyID id)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_values.remove(id);

    m_has_property.set(property_bitmap_index(id), false);
    set_property_inherited(id, ComputedProperties::Inherited::No);
    set_property_result_of_transition(id, AnimatedPropertyResultOfTransition::No);
}

void AnimatedProperties::reset_non_inherited_properties()
{
    for (auto property_id : m_values.keys()) {
        if (!is_property_inherited(property_id))
            remove_property(property_id);
    }
}

bool ComputedProperties::is_property_important(PropertyID property_id) const
{
    return data().property_important.get(property_bitmap_index(property_id));
}

void ComputedProperties::Builder::set_property_important(PropertyID property_id, Important important)
{
    data().property_important.set(property_bitmap_index(property_id), important == Important::Yes);
}

bool ComputedProperties::is_property_inherited(PropertyID property_id) const
{
    return data().property_inherited.get(property_bitmap_index(property_id));
}

HashMap<PropertyID, NonnullRefPtr<StyleValue const>> const& ComputedProperties::animated_property_values() const
{
    return animated_properties().values();
}

RefPtr<AnimatedProperties const> ComputedProperties::animated_properties_snapshot() const
{
    return m_animated_properties;
}

bool ComputedProperties::has_animated_property(PropertyID property_id) const
{
    return animated_properties().has_property(property_id);
}

bool ComputedProperties::is_animated_property_inherited(PropertyID property_id) const
{
    return animated_properties().is_property_inherited(property_id);
}

bool ComputedProperties::is_animated_property_result_of_transition(PropertyID property_id) const
{
    return animated_properties().is_property_result_of_transition(property_id);
}

bool ComputedProperties::has_pseudo_element_style(PseudoElement pseudo_element) const
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return data().pseudo_element_styles & (1ull << to_underlying(pseudo_element));
}

void ComputedProperties::Builder::set_has_pseudo_element_styles(u64 pseudo_element_styles)
{
    constexpr auto known_pseudo_element_count = to_underlying(PseudoElement::KnownPseudoElementCount);
    if constexpr (known_pseudo_element_count < sizeof(u64) * 8)
        VERIFY((pseudo_element_styles >> known_pseudo_element_count) == 0);
    data().pseudo_element_styles |= pseudo_element_styles;
}

void ComputedProperties::Builder::set_property_inherited(PropertyID property_id, Inherited inherited)
{
    data().property_inherited.set(property_bitmap_index(property_id), inherited == Inherited::Yes);
}

void ComputedProperties::Builder::set_depends_on_viewport_metrics()
{
    m_depends_on_viewport_metrics = true;
    m_style->m_depends_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_font_metrics_depend_on_viewport_metrics()
{
    m_font_metrics_depend_on_viewport_metrics = true;
    m_style->m_font_metrics_depend_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_in_display_none_subtree()
{
    m_in_display_none_subtree = true;
    m_style->m_in_display_none_subtree = true;
}

void ComputedProperties::set_depends_on_viewport_metrics(Badge<StyleComputer>)
{
    m_depends_on_viewport_metrics = true;
}

void ComputedProperties::set_font_metrics_depend_on_viewport_metrics(Badge<StyleComputer>)
{
    m_font_metrics_depend_on_viewport_metrics = true;
}

void ComputedProperties::Builder::set_has_pseudo_element_style(PseudoElement pseudo_element)
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    data().pseudo_element_styles |= 1ull << to_underlying(pseudo_element);
}

void ComputedProperties::Builder::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited, Important important)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    set_property_without_modifying_flags(id, move(value));
    set_property_important(id, important);
    set_property_inherited(id, inherited);
}

static bool property_affects_computed_font_list(PropertyID id)
{
    return first_is_one_of(id, PropertyID::FontFamily, PropertyID::FontSize, PropertyID::FontStyle, PropertyID::FontWeight, PropertyID::FontWidth, PropertyID::FontVariationSettings);
}

void ComputedProperties::Builder::set_property_without_modifying_flags(PropertyID id, NonnullRefPtr<StyleValue const> value)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    data().property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = move(value);

    if (property_affects_computed_font_list(id))
        style().clear_computed_font_list_cache();
}

void ComputedProperties::Builder::revert_property(PropertyID id, ComputedProperties const& style_for_revert)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    data().property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = style_for_revert.data().property_values[to_underlying(id) - to_underlying(first_longhand_property_id)];
    set_property_important(id, style_for_revert.is_property_important(id) ? Important::Yes : Important::No);
    set_property_inherited(id, style_for_revert.is_property_inherited(id) ? Inherited::Yes : Inherited::No);

    if (property_affects_computed_font_list(id))
        style().clear_computed_font_list_cache();
}

Display ComputedProperties::display_before_box_type_transformation() const
{
    return data().display_before_box_type_transformation;
}

void ComputedProperties::Builder::set_display_before_box_type_transformation(Display value)
{
    data().display_before_box_type_transformation = value;
}

void ComputedProperties::set_animated_property_internal(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    mutable_animated_properties().set_property(id, move(value), animated_property_result_of_transition, inherited);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedProperties::set_animated_property(Badge<StyleComputer>, PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    set_animated_property_internal(id, move(value), animated_property_result_of_transition, inherited);
}

void ComputedProperties::set_animated_property(Badge<DOM::Element>, PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    set_animated_property_internal(id, move(value), animated_property_result_of_transition, inherited);
}

void ComputedProperties::remove_animated_property(Badge<DOM::Element>, PropertyID id)
{
    if (!has_animated_property(id))
        return;

    bool should_clear_computed_font_list_cache = property_affects_computed_font_list(id);
    auto& animated_properties = mutable_animated_properties();
    animated_properties.remove_property(id);
    if (animated_properties.is_empty())
        m_animated_properties = nullptr;

    if (should_clear_computed_font_list_cache)
        clear_computed_font_list_cache();
}

void ComputedProperties::reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>)
{
    bool has_non_inherited_property = false;
    bool should_clear_computed_font_list_cache = false;
    for (auto const& property : animated_property_values()) {
        if (is_animated_property_inherited(property.key))
            continue;
        has_non_inherited_property = true;
        if (property_affects_computed_font_list(property.key)) {
            should_clear_computed_font_list_cache = true;
            break;
        }
    }

    if (!has_non_inherited_property)
        return;

    auto& animated_properties = mutable_animated_properties();
    animated_properties.reset_non_inherited_properties();
    if (animated_properties.is_empty())
        m_animated_properties = nullptr;

    if (should_clear_computed_font_list_cache)
        clear_computed_font_list_cache();
}

StyleValue const& ComputedProperties::property(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    // Important properties override animated but not transitioned properties
    if (return_animated_value == WithAnimationsApplied::Yes
        && has_animated_property(property_id)
        && (!is_property_important(property_id) || is_animated_property_result_of_transition(property_id))) {
        return animated_properties().property(property_id);
    }

    // By the time we call this method, the property should have been assigned
    return *data().property_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
}

Variant<LengthPercentage, NormalGap> ComputedProperties::gap_value(PropertyID id) const
{
    auto const& value = property(id);
    if (value.is_keyword()) {
        VERIFY(value.as_keyword().keyword() == Keyword::Normal);
        return NormalGap {};
    }

    return LengthPercentage::from_style_value(value);
}

Size ComputedProperties::size_value(PropertyID id) const
{
    return Size::from_style_value(property(id));
}

Length ComputedProperties::length(PropertyID property_id) const
{
    return property(property_id).as_length().length();
}

LengthBox ComputedProperties::length_box(PropertyID left_id, PropertyID top_id, PropertyID right_id, PropertyID bottom_id, LengthPercentageOrAuto const& default_value) const
{
    auto length_box_side = [&](PropertyID id) -> LengthPercentageOrAuto {
        auto const& value = property(id);

        if (value.is_calculated() || value.is_percentage() || value.is_length() || value.has_auto())
            return LengthPercentageOrAuto::from_style_value(value);

        // FIXME: Handle anchor sizes
        return default_value;
    };

    return LengthBox {
        length_box_side(top_id),
        length_box_side(right_id),
        length_box_side(bottom_id),
        length_box_side(left_id)
    };
}

void ComputedProperties::for_each_anchor_name(Function<void(Utf16FlyString const&)> callback) const
{
    auto const& value = property(PropertyID::AnchorName);
    if (value.is_custom_ident()) {
        callback(value.as_custom_ident().custom_ident());
    } else if (value.is_value_list()) {
        for (auto const& item : value.as_value_list().values()) {
            if (item->is_custom_ident())
                callback(item->as_custom_ident().custom_ident());
        }
    }
}

Color ComputedProperties::color(PropertyID id, ColorResolutionContext color_resolution_context) const
{
    return property(id).to_color(color_resolution_context).value();
}

Position ComputedProperties::position_value(PropertyID id) const
{
    auto const& position = property(id).as_position();
    auto const& edge_x = position.edge_x()->as_edge();
    auto const& edge_y = position.edge_y()->as_edge();

    return {
        .offset_x = LengthPercentage::from_style_value(edge_x.offset()),
        .offset_y = LengthPercentage::from_style_value(edge_y.offset()),
    };
}

// https://drafts.csswg.org/css-values-4/#linked-properties
HashMap<PropertyID, StyleValueVector> ComputedProperties::assemble_coordinated_value_list(PropertyID base_property_id, Vector<PropertyID> const& property_ids) const
{
    // A coordinating list property group creates a coordinated value list, which has, for each entry, a value from each
    // property in the group; these are used together to define a single effect, such as a background image layer or an
    // animation. The coordinated value list is assembled as follows:
    // - The length of the coordinated value list is determined by the number of items specified in one particular
    //   coordinating list property, the coordinating list base property. (In the case of backgrounds, this is the
    //   background-image property.)
    // - The Nth value of the coordinated value list is constructed by collecting the Nth use value of each coordinating
    //   list property
    // - If a coordinating list property has too many values specified, excess values at the end of its list are not
    //   used.
    // - If a coordinating list property has too few values specified, its value list is repeated to add more used
    //   values.
    // - The computed values of the coordinating list properties are not affected by such truncation or repetition.
    HashMap<PropertyID, StyleValueVector> coordinated_value_list;

    for (size_t i = 0; i < property(base_property_id).as_value_list().size(); i++) {
        for (auto property_id : property_ids) {
            auto const& list = property(property_id).as_value_list().values();

            coordinated_value_list.ensure(property_id).append(list[i % list.size()]);
        }
    }

    return coordinated_value_list;
}

ColorInterpolation ComputedProperties::color_interpolation() const
{
    auto const& value = property(PropertyID::ColorInterpolation);
    return keyword_to_color_interpolation(value.to_keyword()).value_or(CSS::ColorInterpolation::Auto);
}

ColorInterpolation ComputedProperties::color_interpolation_filters() const
{
    auto const& value = property(PropertyID::ColorInterpolationFilters);
    return keyword_to_color_interpolation(value.to_keyword()).value_or(CSS::ColorInterpolation::Linearrgb);
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
PreferredColorScheme ComputedProperties::color_scheme(PreferredColorScheme preferred_scheme, Optional<Vector<Utf16FlyString> const&> document_supported_schemes) const
{
    // To determine the used color scheme of an element:
    auto const& scheme_value = property(PropertyID::ColorScheme).as_color_scheme();

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if (preferred_scheme != PreferredColorScheme::Auto && scheme_value.schemes().contains_slow(preferred_color_scheme_to_utf16_fly_string(preferred_scheme)))
        return preferred_scheme;

    // 2. Otherwise, if the user has indicated an overriding preference for their chosen color scheme,
    //    and the only keyword is not present in color-scheme for the element,
    //    the user agent must override the color scheme with the user’s preferred color scheme.
    //    See § 2.3 Overriding the Color Scheme.
    // FIXME: We don't currently support setting an "overriding preference" for color schemes.

    // 3. Otherwise, if the user agent supports at least one of the listed color schemes,
    //    the used color scheme is the first supported color scheme in the list.
    auto first_supported = scheme_value.schemes().first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != PreferredColorScheme::Auto; });
    if (first_supported.has_value())
        return preferred_color_scheme_from_string(first_supported.value());

    // 4. Otherwise, the used color scheme is the browser default. (Same as normal.)
    // `normal` indicates that the element supports the page’s supported color schemes, if they are set
    if (document_supported_schemes.has_value()) {
        if (preferred_scheme != PreferredColorScheme::Auto && document_supported_schemes->contains_slow(preferred_color_scheme_to_utf16_fly_string(preferred_scheme)))
            return preferred_scheme;

        auto document_first_supported = document_supported_schemes->first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != PreferredColorScheme::Auto; });
        if (document_first_supported.has_value())
            return preferred_color_scheme_from_string(document_first_supported.value());
    }

    return PreferredColorScheme::Light;
}

NonnullRefPtr<Gfx::Font const> ComputedProperties::font_fallback(bool monospace, bool bold, float point_size)
{
    if (monospace && bold)
        return Platform::FontPlugin::the().default_fixed_width_font().bold_variant();

    if (monospace)
        return Platform::FontPlugin::the().default_fixed_width_font();

    if (bold)
        return Platform::FontPlugin::the().default_font(point_size)->bold_variant();

    return *Platform::FontPlugin::the().default_font(point_size);
}

CSSPixels ComputedProperties::normal_line_height(Gfx::FontPixelMetrics const& font_metrics)
{
    return CSSPixels { round_to<i32>(font_metrics.ascent) + round_to<i32>(font_metrics.descent) };
}

CSSPixels ComputedProperties::line_height(FontComputer const& font_computer) const
{
    // https://drafts.csswg.org/css-inline-3/#line-height-property
    auto const& line_height = property(PropertyID::LineHeight);

    // normal
    // Determine the preferred line height automatically based on font metrics.
    if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
        return normal_line_height(first_available_computed_font(font_computer)->pixel_metrics());

    // <length [0,∞]>
    // The specified length is used as the preferred line height. Negative values are illegal.
    if (line_height.is_length())
        return line_height.as_length().length().absolute_length_to_px();

    // <number [0,∞]>
    // The preferred line height is this number multiplied by the element’s computed font-size.
    if (line_height.is_number())
        return CSSPixels { font_size() * line_height.as_number().number() };

    VERIFY_NOT_REACHED();
}

LineHeightData ComputedProperties::line_height_data(FontComputer const& font_computer) const
{
    auto const& value = property(PropertyID::LineHeight);
    LineHeightData data { .used_value = line_height(font_computer) };

    if (value.is_keyword() && value.to_keyword() == Keyword::Normal) {
        data.computed_value = LineHeightData::Normal {};
    } else if (value.is_number()) {
        data.computed_value = value.as_number().number();
    } else {
        data.computed_value = value.as_length().length();
    }

    return data;
}

Optional<int> ComputedProperties::z_index() const
{
    auto const& value = property(PropertyID::ZIndex);
    if (value.has_auto())
        return {};

    return int_from_style_value(value);
}

float ComputedProperties::opacity() const
{
    return property(PropertyID::Opacity).as_opacity_value().resolved();
}

Optional<SVGPaint> ComputedProperties::fill(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::Fill);

    if (value.to_keyword() == Keyword::None)
        return {};

    return SVGPaint::from_style_value(value, color_resolution_context);
}

float ComputedProperties::fill_opacity() const
{
    return property(PropertyID::FillOpacity).as_opacity_value().resolved();
}

Optional<SVGPaint> ComputedProperties::stroke(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::Stroke);

    if (value.to_keyword() == Keyword::None)
        return {};

    return SVGPaint::from_style_value(value, color_resolution_context);
}

Vector<Variant<LengthPercentage, float>> ComputedProperties::stroke_dasharray() const
{
    auto const& value = property(PropertyID::StrokeDasharray);

    // none
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    auto const& stroke_dasharray = value.as_value_list();
    Vector<Variant<LengthPercentage, float>> dashes;

    for (auto const& value : stroke_dasharray.values()) {
        if (value->is_length()) {
            dashes.append(LengthPercentage { value->as_length().length() });
        } else if (value->is_percentage()) {
            dashes.append(LengthPercentage { value->as_percentage().percentage() });
        } else if (value->is_calculated()) {
            auto const& calculated_value = value->as_calculated();

            if (calculated_value.resolves_to_length())
                dashes.append(LengthPercentage { value->as_calculated() });
            else if (calculated_value.resolves_to_number())
                dashes.append(calculated_value.resolve_number({}).value());
            else
                VERIFY_NOT_REACHED();

        } else if (value->is_number()) {
            dashes.append(value->as_number().number());
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    return dashes;
}

StrokeLinecap ComputedProperties::stroke_linecap() const
{
    auto const& value = property(PropertyID::StrokeLinecap);
    return keyword_to_stroke_linecap(value.to_keyword()).release_value();
}

StrokeLinejoin ComputedProperties::stroke_linejoin() const
{
    auto const& value = property(PropertyID::StrokeLinejoin);
    return keyword_to_stroke_linejoin(value.to_keyword()).release_value();
}

VectorEffect ComputedProperties::vector_effect() const
{
    auto const& value = property(PropertyID::VectorEffect);
    return keyword_to_vector_effect(value.to_keyword()).release_value();
}

double ComputedProperties::stroke_miterlimit() const
{
    return number_from_style_value(property(PropertyID::StrokeMiterlimit), {});
}

float ComputedProperties::stroke_opacity() const
{
    return property(PropertyID::StrokeOpacity).as_opacity_value().resolved();
}

float ComputedProperties::stop_opacity() const
{
    return property(PropertyID::StopOpacity).as_opacity_value().resolved();
}

FillRule ComputedProperties::fill_rule() const
{
    auto const& value = property(PropertyID::FillRule);
    return keyword_to_fill_rule(value.to_keyword()).release_value();
}

ClipRule ComputedProperties::clip_rule() const
{
    auto const& value = property(PropertyID::ClipRule);
    return keyword_to_fill_rule(value.to_keyword()).release_value();
}

float ComputedProperties::flood_opacity() const
{
    return property(PropertyID::FloodOpacity).as_opacity_value().resolved();
}

FlexDirection ComputedProperties::flex_direction() const
{
    auto const& value = property(PropertyID::FlexDirection);
    return keyword_to_flex_direction(value.to_keyword()).release_value();
}

FlexWrap ComputedProperties::flex_wrap() const
{
    auto const& value = property(PropertyID::FlexWrap);
    return keyword_to_flex_wrap(value.to_keyword()).release_value();
}

FlexBasis ComputedProperties::flex_basis() const
{
    auto const& value = property(PropertyID::FlexBasis);

    if (value.is_keyword() && value.to_keyword() == Keyword::Content)
        return FlexBasisContent {};

    return size_value(PropertyID::FlexBasis);
}

double ComputedProperties::flex_grow() const
{
    auto const& value = property(PropertyID::FlexGrow);
    return number_from_style_value(NonnullRefPtr<StyleValue const> { value }, {});
}

double ComputedProperties::flex_shrink() const
{
    auto const& value = property(PropertyID::FlexShrink);
    return number_from_style_value(NonnullRefPtr<StyleValue const> { value }, {});
}

i32 ComputedProperties::order() const
{
    auto const& value = property(PropertyID::Order);
    return int_from_style_value(NonnullRefPtr<StyleValue const> { value });
}

ImageRendering ComputedProperties::image_rendering() const
{
    auto const& value = property(PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword()).release_value();
}

// https://drafts.csswg.org/css-backgrounds-4/#layering
Vector<BackgroundLayerData> ComputedProperties::background_layers() const
{
    auto coordinated_value_list = assemble_coordinated_value_list(
        PropertyID::BackgroundImage,
        {
            PropertyID::BackgroundAttachment,
            PropertyID::BackgroundBlendMode,
            PropertyID::BackgroundClip,
            PropertyID::BackgroundImage,
            PropertyID::BackgroundOrigin,
            PropertyID::BackgroundPositionX,
            PropertyID::BackgroundPositionY,
            PropertyID::BackgroundRepeat,
            PropertyID::BackgroundSize,
        });

    Vector<BackgroundLayerData> layers;
    // The number of layers is determined by the number of comma-separated values in the background-image property
    layers.ensure_capacity(coordinated_value_list.get(PropertyID::BackgroundImage)->size());

    for (size_t i = 0; i < coordinated_value_list.get(PropertyID::BackgroundImage)->size(); i++) {
        auto const& background_image_value = coordinated_value_list.get(PropertyID::BackgroundImage)->at(i);

        auto const& background_attachment_value = coordinated_value_list.get(PropertyID::BackgroundAttachment)->at(i);
        auto const& background_blend_mode_value = coordinated_value_list.get(PropertyID::BackgroundBlendMode)->at(i);
        auto const& background_clip_value = coordinated_value_list.get(PropertyID::BackgroundClip)->at(i);
        auto const& background_origin_value = coordinated_value_list.get(PropertyID::BackgroundOrigin)->at(i);
        auto const& background_position_x_value = coordinated_value_list.get(PropertyID::BackgroundPositionX)->at(i);
        auto const& background_position_y_value = coordinated_value_list.get(PropertyID::BackgroundPositionY)->at(i);
        auto const& background_repeat_value = coordinated_value_list.get(PropertyID::BackgroundRepeat)->at(i);
        auto const& background_size_value = coordinated_value_list.get(PropertyID::BackgroundSize)->at(i);

        BackgroundLayerData layer;
        layer.image_style_value = background_image_value;
        if (background_image_value->is_abstract_image())
            layer.background_image = background_image_value->as_abstract_image();

        layer.attachment = keyword_to_background_attachment(background_attachment_value->to_keyword()).value();
        layer.blend_mode = keyword_to_mix_blend_mode(background_blend_mode_value->to_keyword()).value();
        layer.clip = keyword_to_background_box(background_clip_value->to_keyword()).value();

        layer.origin = keyword_to_background_box(background_origin_value->to_keyword()).value();

        layer.position_x = LengthPercentage::from_style_value(background_position_x_value->as_edge().offset());
        layer.position_y = LengthPercentage::from_style_value(background_position_y_value->as_edge().offset());

        layer.repeat_x = background_repeat_value->as_repeat_style().repeat_x();
        layer.repeat_y = background_repeat_value->as_repeat_style().repeat_y();

        if (background_size_value->is_background_size()) {
            layer.size_type = CSS::BackgroundSize::LengthPercentage;
            layer.size_x = CSS::LengthPercentageOrAuto::from_style_value(background_size_value->as_background_size().size_x());
            layer.size_y = CSS::LengthPercentageOrAuto::from_style_value(background_size_value->as_background_size().size_y());
        } else if (background_size_value->is_keyword()) {
            switch (background_size_value->to_keyword()) {
            case CSS::Keyword::Contain:
                layer.size_type = CSS::BackgroundSize::Contain;
                break;
            case CSS::Keyword::Cover:
                layer.size_type = CSS::BackgroundSize::Cover;
                break;
            default:
                VERIFY_NOT_REACHED();
                break;
            }
        } else {
            VERIFY_NOT_REACHED();
        }

        layers.unchecked_append(layer);
    }

    return layers;
}

BorderImageData ComputedProperties::border_image() const
{
    auto const& source = property(PropertyID::BorderImageSource);
    auto expand_sides = [](StyleValue const& value, auto convert) {
        auto value_at = [&](size_t index) -> StyleValue const& {
            if (value.is_value_list())
                return value.as_value_list().value_at(index, true);
            return value;
        };
        using Value = decltype(convert(value));
        return BorderImageSideValues<Value> { convert(value_at(0)), convert(value_at(1)), convert(value_at(2)), convert(value_at(3)) };
    };

    auto const& repeat = property(PropertyID::BorderImageRepeat);
    auto repeat_at = [&](size_t index) {
        auto const& keyword_value = repeat.is_value_list() ? *repeat.as_value_list().value_at(index, true) : repeat;
        return keyword_to_border_image_repeat(keyword_value.to_keyword()).value_or(BorderImageRepeat::Stretch);
    };

    auto const& slice = property(PropertyID::BorderImageSlice).as_border_image_slice();
    auto component_count = [](StyleValue const& value) -> u8 {
        return value.is_value_list() ? value.as_value_list().size() : 1;
    };
    auto convert_slice = [](StyleValue const& value) -> BorderImageSliceValue {
        if (value.is_percentage())
            return value.as_percentage().percentage();
        if (value.is_calculated())
            return NonnullRefPtr<CalculatedStyleValue const> { value.as_calculated() };
        return value.as_number().number();
    };
    return BorderImageData {
        .source = source.is_abstract_image() ? RefPtr { source.as_abstract_image() } : nullptr,
        .slice = { convert_slice(slice.top()), convert_slice(slice.right()), convert_slice(slice.bottom()), convert_slice(slice.left()) },
        .width = expand_sides(property(PropertyID::BorderImageWidth), [](StyleValue const& value) -> BorderImageWidthValue {
            if (value.is_number())
                return value.as_number().number();
            if (value.to_keyword() == Keyword::Auto)
                return BorderImageWidthAuto {};
            return LengthPercentage::from_style_value(value);
        }),
        .outset = expand_sides(property(PropertyID::BorderImageOutset), [](StyleValue const& value) -> BorderImageOutsetValue {
            if (value.is_number())
                return value.as_number().number();
            return Length::from_style_value(value, {});
        }),
        .width_value_count = component_count(property(PropertyID::BorderImageWidth)),
        .outset_value_count = component_count(property(PropertyID::BorderImageOutset)),
        .fill = slice.fill(),
        .repeat_x = repeat_at(0),
        .repeat_y = repeat_at(1),
    };
}

Vector<BackgroundLayerData> ComputedProperties::mask_layers() const
{
    auto property_values = [&](PropertyID property_id) {
        auto const& value = property(property_id);
        if (value.is_value_list())
            return value.as_value_list().values();
        return StyleValueVector { value };
    };

    auto const mask_image_values = property_values(PropertyID::MaskImage);

    auto mask_clip_values = property_values(PropertyID::MaskClip);
    auto mask_composite_values = property_values(PropertyID::MaskComposite);
    auto mask_mode_values = property_values(PropertyID::MaskMode);
    auto mask_origin_values = property_values(PropertyID::MaskOrigin);
    auto mask_position_values = property_values(PropertyID::MaskPosition);
    auto mask_repeat_values = property_values(PropertyID::MaskRepeat);
    auto mask_size_values = property_values(PropertyID::MaskSize);

    Vector<BackgroundLayerData> layers;
    layers.ensure_capacity(mask_image_values.size());

    for (size_t i = 0; i < mask_image_values.size(); i++) {
        auto const& mask_image_value = mask_image_values[i];

        auto const& mask_clip_value = mask_clip_values[i % mask_clip_values.size()];
        auto const& mask_composite_value = mask_composite_values[i % mask_composite_values.size()];
        auto const& mask_mode_value = mask_mode_values[i % mask_mode_values.size()];
        auto const& mask_origin_value = mask_origin_values[i % mask_origin_values.size()];
        auto const& mask_position_value = mask_position_values[i % mask_position_values.size()];
        auto const& mask_repeat_value = mask_repeat_values[i % mask_repeat_values.size()];
        auto const& mask_size_value = mask_size_values[i % mask_size_values.size()];

        BackgroundLayerData layer;
        layer.image_style_value = mask_image_value;
        layer.origin = BackgroundBox::BorderBox;
        layer.clip = BackgroundBox::BorderBox;
        if (mask_image_value->is_abstract_image())
            layer.background_image = mask_image_value->as_abstract_image();

        if (mask_clip_value->to_keyword() != Keyword::NoClip) {
            layer.mask_clip = keyword_to_coord_box(mask_clip_value->to_keyword()).release_value();
            if (auto clip = keyword_to_background_box(mask_clip_value->to_keyword()); clip.has_value())
                layer.clip = clip.release_value();
        } else {
            layer.mask_clip_is_no_clip = true;
        }

        layer.mask_composite = keyword_to_compositing_operator(mask_composite_value->to_keyword()).release_value();
        layer.mask_mode = keyword_to_masking_mode(mask_mode_value->to_keyword()).release_value();

        layer.mask_origin = keyword_to_coord_box(mask_origin_value->to_keyword()).release_value();
        if (auto origin = keyword_to_background_box(mask_origin_value->to_keyword()); origin.has_value())
            layer.origin = origin.release_value();

        auto const& position = mask_position_value->as_position();
        layer.position_x = LengthPercentage::from_style_value(position.edge_x()->as_edge().offset());
        layer.position_y = LengthPercentage::from_style_value(position.edge_y()->as_edge().offset());

        layer.repeat_x = mask_repeat_value->as_repeat_style().repeat_x();
        layer.repeat_y = mask_repeat_value->as_repeat_style().repeat_y();

        if (mask_size_value->is_background_size()) {
            layer.size_type = CSS::BackgroundSize::LengthPercentage;
            layer.size_x = CSS::LengthPercentageOrAuto::from_style_value(mask_size_value->as_background_size().size_x());
            layer.size_y = CSS::LengthPercentageOrAuto::from_style_value(mask_size_value->as_background_size().size_y());
        } else if (mask_size_value->is_keyword()) {
            switch (mask_size_value->to_keyword()) {
            case CSS::Keyword::Contain:
                layer.size_type = CSS::BackgroundSize::Contain;
                break;
            case CSS::Keyword::Cover:
                layer.size_type = CSS::BackgroundSize::Cover;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        } else {
            VERIFY_NOT_REACHED();
        }

        layers.unchecked_append(layer);
    }

    return layers;
}

BackgroundBox ComputedProperties::background_color_clip() const
{
    // The background color is clipped according to the final layer's background-clip value. We propagate this
    // separately to allow us to avoid computing layer data in the case a layer's `background-image` is `none`

    auto const& background_image_values = property(PropertyID::BackgroundImage).as_value_list().values();
    auto const& background_clip_values = property(PropertyID::BackgroundClip).as_value_list().values();

    // Background clip values are coordinated against background image values so the value used for the final layer is
    // not necessarily the last specified one.
    auto final_layer_index = (background_image_values.size() - 1) % background_clip_values.size();

    return keyword_to_background_box(background_clip_values[final_layer_index]->to_keyword()).value();
}

CSSPixels ComputedProperties::border_spacing_horizontal() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 0);
        return Length::from_style_value(list.value_at(0, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CSSPixels ComputedProperties::border_spacing_vertical() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 1);
        return Length::from_style_value(list.value_at(1, false), {}).absolute_length_to_px();
    }

    return Length::from_style_value(style_value, {}).absolute_length_to_px();
}

CaptionSide ComputedProperties::caption_side() const
{
    auto const& value = property(PropertyID::CaptionSide);
    return keyword_to_caption_side(value.to_keyword()).release_value();
}

Clip ComputedProperties::clip() const
{
    auto const& value = property(PropertyID::Clip);
    if (!value.is_rect())
        return Clip::make_auto();
    return Clip(value.as_rect().rect());
}

JustifyContent ComputedProperties::justify_content() const
{
    auto const& value = property(PropertyID::JustifyContent);
    return keyword_to_justify_content(value.to_keyword()).release_value();
}

JustifyItems ComputedProperties::justify_items() const
{
    auto const& value = property(PropertyID::JustifyItems);
    return keyword_to_justify_items(value.to_keyword()).release_value();
}

JustifySelf ComputedProperties::justify_self() const
{
    auto const& value = property(PropertyID::JustifySelf);
    return keyword_to_justify_self(value.to_keyword()).release_value();
}

Vector<NonnullRefPtr<TransformationStyleValue const>> ComputedProperties::transformations_for_style_value(StyleValue const& value)
{
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    if (!value.is_value_list())
        return {};

    auto& list = value.as_value_list();
    Vector<NonnullRefPtr<TransformationStyleValue const>> transformations;
    for (auto const& transform_value : list.values()) {
        VERIFY(transform_value->is_transformation());
        transformations.append(transform_value->as_transformation());
    }
    return transformations;
}

Vector<NonnullRefPtr<TransformationStyleValue const>> ComputedProperties::transformations() const
{
    return transformations_for_style_value(property(PropertyID::Transform));
}

RefPtr<TransformationStyleValue const> ComputedProperties::rotate() const
{
    auto const& value = property(PropertyID::Rotate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

RefPtr<TransformationStyleValue const> ComputedProperties::translate() const
{
    auto const& value = property(PropertyID::Translate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

RefPtr<TransformationStyleValue const> ComputedProperties::scale() const
{
    auto const& value = property(PropertyID::Scale);
    if (!value.is_transformation())
        return {};
    return value.as_transformation();
}

TransformBox ComputedProperties::transform_box() const
{
    auto const& value = property(PropertyID::TransformBox);
    return keyword_to_transform_box(value.to_keyword()).release_value();
}

Optional<CSSPixels> ComputedProperties::perspective() const
{
    auto const& value = property(PropertyID::Perspective);
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    return Length::from_style_value(value, {}).absolute_length_to_px();
}

Position ComputedProperties::perspective_origin() const
{
    return position_value(PropertyID::PerspectiveOrigin);
}

TransformOrigin ComputedProperties::transform_origin() const
{
    auto length_percentage_with_keywords_resolved = [](StyleValue const& value) -> LengthPercentage {
        if (value.is_keyword()) {
            auto keyword = value.to_keyword();
            if (keyword == Keyword::Left || keyword == Keyword::Top)
                return Percentage(0);
            if (keyword == Keyword::Center)
                return Percentage(50);
            if (keyword == Keyword::Right || keyword == Keyword::Bottom)
                return Percentage(100);

            VERIFY_NOT_REACHED();
        }
        return LengthPercentage::from_style_value(value);
    };

    auto const& value = property(PropertyID::TransformOrigin);
    if (!value.is_value_list() || value.as_value_list().size() != 3)
        return {};
    auto const& list = value.as_value_list();

    auto x_value = length_percentage_with_keywords_resolved(list.values()[0]);
    auto y_value = length_percentage_with_keywords_resolved(list.values()[1]);
    auto z_value = LengthPercentage::from_style_value(list.values()[2]);
    return { x_value, y_value, z_value };
}

TransformStyle ComputedProperties::transform_style() const
{
    auto const& value = property(PropertyID::TransformStyle);
    return keyword_to_transform_style(value.to_keyword()).release_value();
}

Color ComputedProperties::accent_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::AccentColor);

    if (value.to_keyword() == Keyword::Auto)
        return CSS::SystemColor::accent_color(color_resolution_context.color_scheme.value());

    return value.to_color(color_resolution_context).value();
}

AlignContent ComputedProperties::align_content() const
{
    auto const& value = property(PropertyID::AlignContent);
    return keyword_to_align_content(value.to_keyword()).release_value();
}

AlignItems ComputedProperties::align_items() const
{
    auto const& value = property(PropertyID::AlignItems);
    return keyword_to_align_items(value.to_keyword()).release_value();
}

AlignSelf ComputedProperties::align_self() const
{
    auto const& value = property(PropertyID::AlignSelf);
    return keyword_to_align_self(value.to_keyword()).release_value();
}

Appearance ComputedProperties::appearance() const
{
    auto const& value = property(PropertyID::Appearance);
    auto appearance = keyword_to_appearance(value.to_keyword()).release_value();
    switch (appearance) {
    // Note: All these compatibility values can be treated as 'auto'
    case Appearance::Searchfield:
    case Appearance::Textarea:
    case Appearance::PushButton:
    case Appearance::SliderHorizontal:
    case Appearance::Checkbox:
    case Appearance::Radio:
    case Appearance::SquareButton:
    case Appearance::Menulist:
    case Appearance::Listbox:
    case Appearance::Meter:
    case Appearance::ProgressBar:
    case Appearance::Button:
        appearance = Appearance::Auto;
        break;
    // NB: <compat-special> values behave like auto but can also have an effect. Preserve them.
    case Appearance::Textfield:
    case Appearance::MenulistButton:
        break;
    default:
        break;
    }
    return appearance;
}

Filter ComputedProperties::backdrop_filter() const
{
    auto const& value = property(PropertyID::BackdropFilter);
    if (is_filter_style_value_list(value))
        return Filter(value.as_value_list());
    return Filter::make_none();
}

Filter ComputedProperties::filter() const
{
    auto const& value = property(PropertyID::Filter);
    if (is_filter_style_value_list(value))
        return Filter(value.as_value_list());
    return Filter::make_none();
}

Positioning ComputedProperties::position() const
{
    auto const& value = property(PropertyID::Position);
    return keyword_to_positioning(value.to_keyword()).release_value();
}

bool ComputedProperties::operator==(ComputedProperties const& other) const
{
    for (size_t i = 0; i < data().property_values.size(); ++i) {
        auto const& my_style = data().property_values[i];
        auto const& other_style = other.data().property_values[i];
        if (!my_style) {
            if (other_style)
                return false;
            continue;
        }
        if (!other_style)
            return false;
        auto const& my_value = *my_style;
        auto const& other_value = *other_style;
        if (my_value.type() != other_value.type())
            return false;
        if (my_value != other_value)
            return false;
    }

    return true;
}

TextAnchor ComputedProperties::text_anchor() const
{
    auto const& value = property(PropertyID::TextAnchor);
    return keyword_to_text_anchor(value.to_keyword()).release_value();
}

Optional<BaselineMetric> ComputedProperties::dominant_baseline() const
{
    auto const& value = property(PropertyID::DominantBaseline);
    return keyword_to_baseline_metric(value.to_keyword());
}

TextAlign ComputedProperties::text_align() const
{
    auto const& value = property(PropertyID::TextAlign);
    return keyword_to_text_align(value.to_keyword()).release_value();
}

TextJustify ComputedProperties::text_justify() const
{
    auto const& value = property(PropertyID::TextJustify);
    return keyword_to_text_justify(value.to_keyword()).release_value();
}

TextOverflow ComputedProperties::text_overflow() const
{
    auto const& value = property(PropertyID::TextOverflow);
    return keyword_to_text_overflow(value.to_keyword()).release_value();
}

TextRendering ComputedProperties::text_rendering() const
{
    auto const& value = property(PropertyID::TextRendering);
    return keyword_to_text_rendering(value.to_keyword()).release_value();
}

CSSPixels ComputedProperties::text_underline_offset() const
{
    auto const& computed_text_underline_offset = property(PropertyID::TextUnderlineOffset);

    // auto
    if (computed_text_underline_offset.to_keyword() == Keyword::Auto)
        return InitialValues::text_underline_offset();

    // <length>
    // <percentage>
    return Length::from_style_value(computed_text_underline_offset, Length::make_px(font_size())).absolute_length_to_px();
}

TextUnderlinePosition ComputedProperties::text_underline_position() const
{
    auto const& computed_text_underline_position = property(PropertyID::TextUnderlinePosition).as_text_underline_position();

    return {
        .horizontal = computed_text_underline_position.horizontal(),
        .vertical = computed_text_underline_position.vertical()
    };
}

PointerEvents ComputedProperties::pointer_events() const
{
    auto const& value = property(PropertyID::PointerEvents);
    return keyword_to_pointer_events(value.to_keyword()).release_value();
}

Variant<CSSPixels, double> ComputedProperties::tab_size() const
{
    auto const& value = property(PropertyID::TabSize);
    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        if (math_value.resolves_to_length()) {
            return math_value.resolve_length({}).value().absolute_length_to_px();
        }
        if (math_value.resolves_to_number()) {
            return math_value.resolve_number({}).value();
        }
    }

    if (value.is_length())
        return value.as_length().length().absolute_length_to_px();

    return value.as_number().number();
}

WordBreak ComputedProperties::word_break() const
{
    auto const& value = property(PropertyID::WordBreak);
    return keyword_to_word_break(value.to_keyword()).release_value();
}

CSSPixels ComputedProperties::word_spacing() const
{
    auto const& value = property(PropertyID::WordSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

WhiteSpaceCollapse ComputedProperties::white_space_collapse() const
{
    auto const& value = property(PropertyID::WhiteSpaceCollapse);
    return keyword_to_white_space_collapse(value.to_keyword()).release_value();
}

WhiteSpaceTrimData ComputedProperties::white_space_trim() const
{
    auto const& value = property(PropertyID::WhiteSpaceTrim);

    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return WhiteSpaceTrimData {};

    if (value.is_value_list()) {
        auto white_space_trim_data = WhiteSpaceTrimData {};

        for (auto const& value : value.as_value_list().values()) {
            switch (value->as_keyword().keyword()) {
            case Keyword::DiscardBefore:
                white_space_trim_data.discard_before = true;
                break;
            case Keyword::DiscardAfter:
                white_space_trim_data.discard_after = true;
                break;
            case Keyword::DiscardInner:
                white_space_trim_data.discard_inner = true;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }

        return white_space_trim_data;
    }

    VERIFY_NOT_REACHED();
}

CSSPixels ComputedProperties::letter_spacing() const
{
    auto const& value = property(PropertyID::LetterSpacing);
    if (value.is_keyword() && value.to_keyword() == Keyword::Normal)
        return 0;

    return Length::from_style_value(value, Length::make_px(font_size())).absolute_length_to_px();
}

LineStyle ComputedProperties::line_style(PropertyID property_id) const
{
    auto const& value = property(property_id);
    return keyword_to_line_style(value.to_keyword()).release_value();
}

OutlineStyle ComputedProperties::outline_style() const
{
    auto const& value = property(PropertyID::OutlineStyle);
    return keyword_to_outline_style(value.to_keyword()).release_value();
}

Float ComputedProperties::float_() const
{
    auto const& value = property(PropertyID::Float);
    return keyword_to_float(value.to_keyword()).release_value();
}

Color ComputedProperties::caret_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::CaretColor);
    if (value.is_keyword() && value.to_keyword() == Keyword::Auto)
        return color_resolution_context.current_color.value_or(InitialValues::color());

    if (value.has_color())
        return value.to_color(color_resolution_context).value();

    return InitialValues::caret_color();
}

Clear ComputedProperties::clear() const
{
    auto const& value = property(PropertyID::Clear);
    return keyword_to_clear(value.to_keyword()).release_value();
}

ColumnSpan ComputedProperties::column_span() const
{
    auto const& value = property(PropertyID::ColumnSpan);
    return keyword_to_column_span(value.to_keyword()).release_value();
}

static ContentDataAndQuoteNestingLevel resolve_content(StyleValue const& value, QuotesData const& quotes_data, DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level)
{
    auto quote_nesting_level = initial_quote_nesting_level;

    auto get_quote_string = [&](bool open, auto depth) {
        switch (quotes_data.type) {
        case QuotesData::Type::None:
            return Utf16FlyString {};
        case QuotesData::Type::Auto:
            // FIXME: "A typographically appropriate used value for quotes is automatically chosen by the UA
            //        based on the content language of the element and/or its parent."
            if (open)
                return depth == 0 ? u"“"_utf16_fly_string : u"‘"_utf16_fly_string;
            return depth == 0 ? u"”"_utf16_fly_string : u"’"_utf16_fly_string;
        case QuotesData::Type::Specified:
            // If the depth is greater than the number of pairs, the last pair is repeated.
            auto& level = quotes_data.strings[min(depth, quotes_data.strings.size() - 1)];
            return open ? level[0] : level[1];
        }
        VERIFY_NOT_REACHED();
    };

    if (value.is_content()) {
        auto& content_style_value = value.as_content();

        ContentData content_data;

        for (auto const& item : content_style_value.content().values()) {
            if (item->is_string()) {
                content_data.data.append(item->as_string().string_value().to_utf16_string());
            } else if (item->is_keyword()) {
                switch (item->to_keyword()) {
                case Keyword::OpenQuote:
                    content_data.data.append(get_quote_string(true, quote_nesting_level++).to_utf16_string());
                    break;
                case Keyword::CloseQuote:
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    if (quote_nesting_level > 0)
                        content_data.data.append(get_quote_string(false, --quote_nesting_level).to_utf16_string());
                    break;
                case Keyword::NoOpenQuote:
                    quote_nesting_level++;
                    break;
                case Keyword::NoCloseQuote:
                    // NOTE: See CloseQuote
                    if (quote_nesting_level > 0)
                        quote_nesting_level--;
                    break;
                default:
                    dbgln("`{}` is not supported in `content` (yet?)", item->to_string(SerializationMode::Normal));
                    break;
                }
            } else if (item->is_counter()) {
                content_data.counter_style_dependencies.append(item->as_counter().counter_style()->as_counter_style().resolve_counter_style(element_reference.style_scope()));
                content_data.data.append(item->as_counter().resolve(element_reference));
            } else if (item->is_image() || item->is_image_set()) {
                // https://drafts.csswg.org/css-content-3/#typedef-content-list
                // https://drafts.csswg.org/css-images-4/#typedef-image
                // <content-list> accepts <image>, and image-set() is an <image>.
                content_data.data.append(NonnullRefPtr { const_cast<AbstractImageStyleValue&>(item->as_abstract_image()) });
            } else {
                // TODO: Implement images, and other things.
                dbgln("`{}` is not supported in `content` (yet?)", item->to_string(SerializationMode::Normal));
            }
        }
        content_data.type = ContentData::Type::List;

        if (auto alt_text = content_style_value.alt_text()) {
            Utf16StringBuilder alt_text_builder;
            for (auto const& item : alt_text->values()) {
                if (item->is_string()) {
                    alt_text_builder.append(item->as_string().string_value().view());
                } else if (item->is_counter()) {
                    content_data.counter_style_dependencies.append(item->as_counter().counter_style()->as_counter_style().resolve_counter_style(element_reference.style_scope()));
                    alt_text_builder.append(item->as_counter().resolve(element_reference));
                } else {
                    dbgln("`{}` is not supported in `content` alt-text (yet?)", item->to_string(SerializationMode::Normal));
                }
            }
            content_data.alt_text = alt_text_builder.to_string();
        }

        return { content_data, quote_nesting_level };
    }

    switch (value.to_keyword()) {
    case Keyword::None:
        return { { ContentData::Type::None, {}, {} }, quote_nesting_level };
    case Keyword::Normal:
        return { { ContentData::Type::Normal, {}, {} }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
}

ContentDataAndQuoteNestingLevel ComputedProperties::content(DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level) const
{
    return resolve_content(property(PropertyID::Content), quotes(), element_reference, initial_quote_nesting_level);
}

ContentDataAndQuoteNestingLevel ComputedValues::resolved_content(DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level) const
{
    return resolve_content(*computed_style_value(PropertyID::Content), quotes(), element_reference, initial_quote_nesting_level);
}

ContentVisibility ComputedProperties::content_visibility() const
{
    auto const& value = property(PropertyID::ContentVisibility);
    return keyword_to_content_visibility(value.to_keyword()).release_value();
}

Vector<CursorData> ComputedProperties::cursor() const
{
    // Return the first available cursor.
    auto const& value = property(PropertyID::Cursor);
    Vector<CursorData> cursors;
    if (value.is_value_list()) {
        for (auto const& item : value.as_value_list().values()) {
            if (item->is_cursor()) {
                cursors.append({ item->as_cursor() });
                continue;
            }

            if (auto keyword = keyword_to_cursor_predefined(item->to_keyword()); keyword.has_value())
                cursors.append(keyword.release_value());
        }
    } else if (value.is_keyword()) {
        if (auto keyword = keyword_to_cursor_predefined(value.to_keyword()); keyword.has_value())
            cursors.append(keyword.release_value());
    }

    if (cursors.is_empty())
        cursors.append(CursorPredefined::Auto);

    return cursors;
}

Visibility ComputedProperties::visibility() const
{
    auto const& value = property(PropertyID::Visibility);
    if (!value.is_keyword())
        return {};
    return keyword_to_visibility(value.to_keyword()).release_value();
}

Display ComputedProperties::display() const
{
    return property(PropertyID::Display).as_display().display();
}

Vector<TextDecorationLine> ComputedProperties::text_decoration_line() const
{
    auto const& value = property(PropertyID::TextDecorationLine);

    if (value.to_keyword() == Keyword::None)
        return {};

    if (value.is_value_list()) {
        Vector<TextDecorationLine> lines;
        auto& values = value.as_value_list().values();
        for (auto const& item : values) {
            lines.append(keyword_to_text_decoration_line(item->to_keyword()).value());
        }
        return lines;
    }

    VERIFY_NOT_REACHED();
}

TextDecorationSkipInk ComputedProperties::text_decoration_skip_ink() const
{
    auto const& value = property(PropertyID::TextDecorationSkipInk);
    return keyword_to_text_decoration_skip_ink(value.to_keyword()).release_value();
}

TextDecorationStyle ComputedProperties::text_decoration_style() const
{
    auto const& value = property(PropertyID::TextDecorationStyle);
    return keyword_to_text_decoration_style(value.to_keyword()).release_value();
}

TextDecorationThickness ComputedProperties::text_decoration_thickness() const
{
    auto const& value = property(PropertyID::TextDecorationThickness);
    if (value.is_keyword()) {
        switch (value.to_keyword()) {
        case Keyword::Auto:
            return { TextDecorationThickness::Auto {} };
        case Keyword::FromFont:
            return { TextDecorationThickness::FromFont {} };
        default:
            VERIFY_NOT_REACHED();
        }
    }

    return TextDecorationThickness { LengthPercentage::from_style_value(value) };
}

TextTransform ComputedProperties::text_transform() const
{
    auto const& value = property(PropertyID::TextTransform);
    return keyword_to_text_transform(value.to_keyword()).release_value();
}

ListStyleType ComputedProperties::list_style_type(StyleScope const& style_scope) const
{
    auto const& value = property(PropertyID::ListStyleType);

    if (value.to_keyword() == Keyword::None)
        return Empty {};

    if (value.is_string())
        return value.as_string().string_value().to_utf16_string();

    auto counter_style = value.as_counter_style().resolve_counter_style(style_scope);
    if (counter_style)
        return counter_style;

    VERIFY(value.as_counter_style().value().has<Utf16FlyString>());
    return value.as_counter_style().value().get<Utf16FlyString>();
}

ListStylePosition ComputedProperties::list_style_position() const
{
    auto const& value = property(PropertyID::ListStylePosition);
    return keyword_to_list_style_position(value.to_keyword()).release_value();
}

Overflow ComputedProperties::overflow_x() const
{
    return overflow(PropertyID::OverflowX);
}

Overflow ComputedProperties::overflow_y() const
{
    return overflow(PropertyID::OverflowY);
}

Overflow ComputedProperties::overflow(PropertyID property_id) const
{
    auto const& value = property(property_id);
    return keyword_to_overflow(value.to_keyword()).release_value();
}

Vector<ShadowData> ComputedProperties::shadow(PropertyID property_id, ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(property_id);

    auto make_shadow_data = [&color_resolution_context](ShadowStyleValue const& value) -> Optional<ShadowData> {
        auto offset_x = Length::from_style_value(value.offset_x(), {}).absolute_length_to_px();
        auto offset_y = Length::from_style_value(value.offset_y(), {}).absolute_length_to_px();
        auto blur_radius = Length::from_style_value(value.blur_radius(), {}).absolute_length_to_px();
        auto spread_distance = Length::from_style_value(value.spread_distance(), {}).absolute_length_to_px();
        auto color_syntax = [&] {
            if (!value.color()->is_color())
                return ColorSyntax::Legacy;
            auto const& color = value.color()->as_color();
            if (!color.color_type().has_value())
                return color.color_syntax();
            switch (*color.color_type()) {
            case ColorStyleValue::ColorType::RGB:
            case ColorStyleValue::ColorType::HSL:
            case ColorStyleValue::ColorType::HWB:
                return ColorSyntax::Legacy;
            default:
                return ColorSyntax::Modern;
            }
        }();
        return ShadowData {
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            value.color()->to_color(color_resolution_context).value(),
            color_syntax,
            value.placement()
        };
    };

    if (value.to_keyword() == Keyword::None)
        return {};

    auto const& value_list = value.as_value_list();

    Vector<ShadowData> shadow_data;
    shadow_data.ensure_capacity(value_list.size());
    for (auto const& layer_value : value_list.values()) {
        auto maybe_shadow_data = make_shadow_data(layer_value->as_shadow());
        if (!maybe_shadow_data.has_value())
            return {};
        shadow_data.append(maybe_shadow_data.release_value());
    }

    return shadow_data;
}

Vector<ShadowData> ComputedProperties::box_shadow(ColorResolutionContext const& color_resolution_context) const
{
    return shadow(PropertyID::BoxShadow, color_resolution_context);
}

Vector<ShadowData> ComputedProperties::text_shadow(ColorResolutionContext const& color_resolution_context) const
{
    return shadow(PropertyID::TextShadow, color_resolution_context);
}

TextIndentData ComputedProperties::text_indent() const
{
    auto const& value = property(PropertyID::TextIndent).as_text_indent();

    return TextIndentData {
        .length_percentage = LengthPercentage::from_style_value(value.length_percentage()),
        .each_line = value.each_line(),
        .hanging = value.hanging(),
    };
}

TextWrapMode ComputedProperties::text_wrap_mode() const
{
    auto const& value = property(PropertyID::TextWrapMode);
    return keyword_to_text_wrap_mode(value.to_keyword()).release_value();
}

BoxSizing ComputedProperties::box_sizing() const
{
    auto const& value = property(PropertyID::BoxSizing);
    return keyword_to_box_sizing(value.to_keyword()).release_value();
}

Variant<VerticalAlign, LengthPercentage> ComputedProperties::vertical_align() const
{
    auto const& value = property(PropertyID::VerticalAlign);

    if (value.is_keyword())
        return keyword_to_vertical_align(value.to_keyword()).release_value();

    return LengthPercentage::from_style_value(value);
}

FontKerning ComputedProperties::font_kerning() const
{
    auto const& value = property(PropertyID::FontKerning);
    return keyword_to_font_kerning(value.to_keyword()).release_value();
}

Optional<Utf16FlyString> ComputedProperties::font_language_override() const
{
    auto const& value = property(PropertyID::FontLanguageOverride);
    if (value.is_string())
        return value.as_string().string_value();
    return {};
}

FontFeatureData ComputedProperties::font_feature_data() const
{
    return {
        .font_variant_alternates = font_variant_alternates(),
        .font_variant_caps = font_variant_caps(),
        .font_variant_east_asian = font_variant_east_asian(),
        .font_variant_emoji = font_variant_emoji(),
        .font_variant_ligatures = font_variant_ligatures(),
        .font_variant_numeric = font_variant_numeric(),
        .font_variant_position = font_variant_position(),
        .font_feature_settings = font_feature_settings(),
        .font_kerning = font_kerning(),
        .text_rendering = text_rendering(),
    };
}

Optional<FontVariantAlternates> ComputedProperties::font_variant_alternates() const
{
    auto const& value = property(PropertyID::FontVariantAlternates);

    // normal
    if (value.is_keyword()) {
        VERIFY(value.to_keyword() == Keyword::Normal);
        return {};
    }

    FontVariantAlternates alternates;

    for (auto const& value : value.as_value_list().values()) {
        // historical-forms
        if (value->is_keyword() && value->to_keyword() == Keyword::HistoricalForms) {
            alternates.historical_forms = true;
            continue;
        }

        if (value->is_function()) {
            auto function_type = font_feature_value_type_from_string(value->as_function().name()).release_value();
            auto const& names = value->as_function().value()->as_value_list().values();

            for (auto const& name : names)
                alternates.font_feature_value_entries.append({ function_type, string_from_style_value(name) });

            continue;
        }

        VERIFY_NOT_REACHED();
    }

    return alternates;
}

FontVariantCaps ComputedProperties::font_variant_caps() const
{
    auto const& value = property(PropertyID::FontVariantCaps);
    return keyword_to_font_variant_caps(value.to_keyword()).release_value();
}

Optional<FontVariantEastAsian> ComputedProperties::font_variant_east_asian() const
{
    auto const& value = property(PropertyID::FontVariantEastAsian);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    auto const& tuple = value.as_tuple().tuple();

    FontVariantEastAsian east_asian {};

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Variant])
        east_asian.variant = keyword_to_east_asian_variant(tuple[TupleStyleValue::Indices::FontVariantEastAsian::Variant]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Width])
        east_asian.width = keyword_to_east_asian_width(tuple[TupleStyleValue::Indices::FontVariantEastAsian::Width]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantEastAsian::Ruby])
        east_asian.ruby = true;

    return east_asian;
}

FontVariantEmoji ComputedProperties::font_variant_emoji() const
{
    auto const& value = property(PropertyID::FontVariantEmoji);
    return keyword_to_font_variant_emoji(value.to_keyword()).release_value();
}

Optional<FontVariantLigatures> ComputedProperties::font_variant_ligatures() const
{
    auto const& value = property(PropertyID::FontVariantLigatures);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    if (value.to_keyword() == Keyword::None)
        return FontVariantLigatures { .none = true };

    auto const& tuple = value.as_tuple().tuple();

    FontVariantLigatures ligatures {};

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Common])
        ligatures.common = keyword_to_common_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Common]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Discretionary])
        ligatures.discretionary = keyword_to_discretionary_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Discretionary]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Historical])
        ligatures.historical = keyword_to_historical_lig_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Historical]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantLigatures::Contextual])
        ligatures.contextual = keyword_to_contextual_alt_value(tuple[TupleStyleValue::Indices::FontVariantLigatures::Contextual]->to_keyword()).value();

    return ligatures;
}

Optional<FontVariantNumeric> ComputedProperties::font_variant_numeric() const
{
    auto const& value = property(PropertyID::FontVariantNumeric);

    if (value.to_keyword() == Keyword::Normal)
        return {};

    auto const& tuple = value.as_tuple().tuple();

    FontVariantNumeric numeric {};

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Figure])
        numeric.figure = keyword_to_numeric_figure_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Figure]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Spacing])
        numeric.spacing = keyword_to_numeric_spacing_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Spacing]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Fraction])
        numeric.fraction = keyword_to_numeric_fraction_value(tuple[TupleStyleValue::Indices::FontVariantNumeric::Fraction]->to_keyword()).value();

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::Ordinal])
        numeric.ordinal = true;

    if (tuple[TupleStyleValue::Indices::FontVariantNumeric::SlashedZero])
        numeric.slashed_zero = true;

    return numeric;
}

FontVariantPosition ComputedProperties::font_variant_position() const
{
    auto const& value = property(PropertyID::FontVariantPosition);
    return keyword_to_font_variant_position(value.to_keyword()).release_value();
}

HashMap<Utf16FlyString, u8> ComputedProperties::font_feature_settings() const
{
    auto const& value = property(PropertyID::FontFeatureSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& feature_tags = value.as_value_list().values();
        HashMap<Utf16FlyString, u8> result;
        result.ensure_capacity(feature_tags.size());
        for (auto const& tag_value : feature_tags) {
            auto const& feature_tag = tag_value->as_open_type_tagged();

            result.set(feature_tag.tag(), int_from_style_value(feature_tag.value()));
        }
        return result;
    }

    return {};
}

HashMap<Utf16FlyString, double> ComputedProperties::font_variation_settings() const
{
    auto const& value = property(PropertyID::FontVariationSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& axis_tags = value.as_value_list().values();
        HashMap<Utf16FlyString, double> result;
        result.ensure_capacity(axis_tags.size());
        for (auto const& tag_value : axis_tags) {
            auto const& axis_tag = tag_value->as_open_type_tagged();

            result.set(axis_tag.tag(), number_from_style_value(axis_tag.value(), {}));
        }
        return result;
    }

    return {};
}

GridTrackSizeList ComputedProperties::grid_auto_columns() const
{
    auto const& value = property(PropertyID::GridAutoColumns);
    return value.as_grid_track_size_list().grid_track_size_list();
}

GridTrackSizeList ComputedProperties::grid_auto_rows() const
{
    auto const& value = property(PropertyID::GridAutoRows);
    return value.as_grid_track_size_list().grid_track_size_list();
}

GridTrackSizeList ComputedProperties::grid_template_columns() const
{
    auto const& value = property(PropertyID::GridTemplateColumns);
    return value.as_grid_track_size_list().grid_track_size_list();
}

GridTrackSizeList ComputedProperties::grid_template_rows() const
{
    auto const& value = property(PropertyID::GridTemplateRows);
    return value.as_grid_track_size_list().grid_track_size_list();
}

GridAutoFlow ComputedProperties::grid_auto_flow() const
{
    auto const& value = property(PropertyID::GridAutoFlow);
    if (!value.is_grid_auto_flow())
        return GridAutoFlow {};
    auto& grid_auto_flow_value = value.as_grid_auto_flow();
    return GridAutoFlow { .row = grid_auto_flow_value.is_row(), .dense = grid_auto_flow_value.is_dense() };
}

GridTrackPlacement ComputedProperties::grid_column_end() const
{
    auto const& value = property(PropertyID::GridColumnEnd);
    return value.as_grid_track_placement().grid_track_placement();
}

GridTrackPlacement ComputedProperties::grid_column_start() const
{
    auto const& value = property(PropertyID::GridColumnStart);
    return value.as_grid_track_placement().grid_track_placement();
}

GridTrackPlacement ComputedProperties::grid_row_end() const
{
    auto const& value = property(PropertyID::GridRowEnd);
    return value.as_grid_track_placement().grid_track_placement();
}

GridTrackPlacement ComputedProperties::grid_row_start() const
{
    auto const& value = property(PropertyID::GridRowStart);
    return value.as_grid_track_placement().grid_track_placement();
}

BorderCollapse ComputedProperties::border_collapse() const
{
    auto const& value = property(PropertyID::BorderCollapse);
    return keyword_to_border_collapse(value.to_keyword()).release_value();
}

EmptyCells ComputedProperties::empty_cells() const
{
    auto const& value = property(PropertyID::EmptyCells);
    return keyword_to_empty_cells(value.to_keyword()).release_value();
}

GridTemplateAreas ComputedProperties::grid_template_areas() const
{
    auto const& value = property(PropertyID::GridTemplateAreas);
    auto const& style_value = value.as_grid_template_area();
    return { style_value.grid_areas(), style_value.row_count(), style_value.column_count() };
}

ObjectFit ComputedProperties::object_fit() const
{
    auto const& value = property(PropertyID::ObjectFit);
    return keyword_to_object_fit(value.to_keyword()).release_value();
}

Position ComputedProperties::object_position() const
{
    return position_value(PropertyID::ObjectPosition);
}

TableLayout ComputedProperties::table_layout() const
{
    auto const& value = property(PropertyID::TableLayout);
    return keyword_to_table_layout(value.to_keyword()).release_value();
}

Direction ComputedProperties::direction() const
{
    auto const& value = property(PropertyID::Direction);
    return keyword_to_direction(value.to_keyword()).release_value();
}

UnicodeBidi ComputedProperties::unicode_bidi() const
{
    auto const& value = property(PropertyID::UnicodeBidi);
    return keyword_to_unicode_bidi(value.to_keyword()).release_value();
}

WritingMode ComputedProperties::writing_mode() const
{
    auto const& value = property(PropertyID::WritingMode);
    return keyword_to_writing_mode(value.to_keyword()).release_value();
}

UserSelect ComputedProperties::user_select() const
{
    auto const& value = property(PropertyID::UserSelect);
    return keyword_to_user_select(value.to_keyword()).release_value();
}

Isolation ComputedProperties::isolation() const
{
    auto const& value = property(PropertyID::Isolation);
    return keyword_to_isolation(value.to_keyword()).release_value();
}

TouchActionData ComputedProperties::touch_action() const
{
    auto const& touch_action = property(PropertyID::TouchAction);
    if (touch_action.is_keyword()) {
        switch (touch_action.to_keyword()) {
        case Keyword::Auto:
            return TouchActionData {};
        case Keyword::None:
            return TouchActionData::none();
        case Keyword::Manipulation:
            return TouchActionData { .allow_other = false };
        default:
            VERIFY_NOT_REACHED();
        }
    }
    if (touch_action.is_value_list()) {
        TouchActionData touch_action_data = TouchActionData::none();
        for (auto const& value : touch_action.as_value_list().values()) {
            switch (value->as_keyword().keyword()) {
            case Keyword::PanX:
                touch_action_data.allow_right = true;
                touch_action_data.allow_left = true;
                break;
            case Keyword::PanLeft:
                touch_action_data.allow_left = true;
                break;
            case Keyword::PanRight:
                touch_action_data.allow_right = true;
                break;
            case Keyword::PanY:
                touch_action_data.allow_up = true;
                touch_action_data.allow_down = true;
                break;
            case Keyword::PanUp:
                touch_action_data.allow_up = true;
                break;
            case Keyword::PanDown:
                touch_action_data.allow_down = true;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
        }
        return touch_action_data;
    }
    return TouchActionData {};
}

Containment ComputedProperties::contain() const
{
    Containment containment = {};
    auto const& value = property(PropertyID::Contain);

    switch (value.to_keyword()) {
    case Keyword::None:
        // This value indicates that the property has no effect. The element renders as normal, with no containment effects applied.
        return {};
    case Keyword::Strict:
        // This value computes to 'size layout paint style', and thus turns on all forms of containment for the element.
        containment.size_containment = true;
        containment.layout_containment = true;
        containment.paint_containment = true;
        containment.style_containment = true;
        break;
    case Keyword::Content:
        //  This value computes to 'layout paint style', and thus turns on all forms of containment except size containment for the element.
        containment.layout_containment = true;
        containment.paint_containment = true;
        containment.style_containment = true;
        break;
    case Keyword::Size:
        containment.size_containment = true;
        break;
    case Keyword::InlineSize:
        containment.inline_size_containment = true;
        break;
    case Keyword::Layout:
        containment.layout_containment = true;
        break;
    case Keyword::Style:
        containment.style_containment = true;
        break;
    case Keyword::Paint:
        containment.paint_containment = true;
        break;
    default:
        if (value.is_value_list()) {
            auto& values = value.as_value_list().values();
            for (auto const& item : values) {
                switch (item->to_keyword()) {
                case Keyword::Size:
                    containment.size_containment = true;
                    break;
                case Keyword::InlineSize:
                    containment.inline_size_containment = true;
                    break;
                case Keyword::Layout:
                    containment.layout_containment = true;
                    break;
                case Keyword::Style:
                    containment.style_containment = true;
                    break;
                case Keyword::Paint:
                    containment.paint_containment = true;
                    break;
                default:
                    dbgln("`{}` is not supported in `contain` (yet?)", item->to_string(SerializationMode::Normal));
                    break;
                }
            }
        }
    }

    return containment;
}

Vector<Utf16FlyString> ComputedProperties::container_name() const
{
    auto const& value = property(PropertyID::ContainerName);
    if (value.to_keyword() == Keyword::None)
        return {};

    Vector<Utf16FlyString> names;

    if (value.is_value_list()) {
        auto& values = value.as_value_list().values();
        for (auto const& item : values)
            names.append(item->as_custom_ident().custom_ident());
    } else {
        names.append(value.as_custom_ident().custom_ident());
    }

    return names;
}

ContainerType ComputedProperties::container_type() const
{
    ContainerType container_type {};

    auto const& value = property(PropertyID::ContainerType);

    if (value.to_keyword() == Keyword::Normal)
        return container_type;

    if (value.is_value_list()) {
        auto& values = value.as_value_list().values();
        for (auto const& item : values) {
            switch (item->to_keyword()) {
            case Keyword::Size:
                container_type.is_size_container = true;
                break;
            case Keyword::InlineSize:
                container_type.is_inline_size_container = true;
                break;
            case Keyword::ScrollState:
                container_type.is_scroll_state_container = true;
                break;
            default:
                dbgln("`{}` is not supported in `container-type` (yet?)", item->to_string(SerializationMode::Normal));
                break;
            }
        }
    }

    return container_type;
}

MixBlendMode ComputedProperties::mix_blend_mode() const
{
    auto const& value = property(PropertyID::MixBlendMode);
    return keyword_to_mix_blend_mode(value.to_keyword()).release_value();
}

Optional<Utf16FlyString> ComputedProperties::view_transition_name() const
{
    auto const& value = property(PropertyID::ViewTransitionName);
    if (value.is_custom_ident())
        return value.as_custom_ident().custom_ident();
    return {};
}

Vector<AnimationProperties> ComputedProperties::animations(DOM::AbstractElement const& abstract_element) const
{
    auto const& animation_name_values = property(PropertyID::AnimationName).as_value_list().values();

    // OPTIMIZATION: If all animation names are 'none', there are no animations to process
    if (all_of(animation_name_values, [](auto const& value) { return value->to_keyword() == Keyword::None; }))
        return {};

    // CSS Animations are defined by binding keyframes to an element using the animation-* properties. These list-valued
    // properties, which are all longhands of the animation shorthand, form a coordinating list property group with
    // animation-name as the coordinating list base property and each item in the coordinated value list defining the
    // properties of a single animation effect.
    auto const& coordinated_properties = assemble_coordinated_value_list(
        PropertyID::AnimationName,
        { PropertyID::AnimationDuration,
            PropertyID::AnimationTimingFunction,
            PropertyID::AnimationIterationCount,
            PropertyID::AnimationDirection,
            PropertyID::AnimationPlayState,
            PropertyID::AnimationDelay,
            PropertyID::AnimationFillMode,
            PropertyID::AnimationComposition,
            PropertyID::AnimationName,
            PropertyID::AnimationTimeline });

    Vector<AnimationProperties> animations;

    for (size_t i = 0; i < coordinated_properties.get(PropertyID::AnimationName)->size(); i++) {
        // https://drafts.csswg.org/css-animations-1/#propdef-animation-name
        // none: No keyframes are specified at all, so there will be no animation. Any other animations properties
        //       specified for this animation have no effect.
        if (coordinated_properties.get(PropertyID::AnimationName).value()[i]->to_keyword() == Keyword::None)
            continue;

        auto animation_name_style_value = coordinated_properties.get(PropertyID::AnimationName).value()[i];
        auto animation_duration_style_value = coordinated_properties.get(PropertyID::AnimationDuration).value()[i];
        auto animation_timing_function_style_value = coordinated_properties.get(PropertyID::AnimationTimingFunction).value()[i];
        auto animation_iteration_count_style_value = coordinated_properties.get(PropertyID::AnimationIterationCount).value()[i];
        auto animation_direction_style_value = coordinated_properties.get(PropertyID::AnimationDirection).value()[i];
        auto animation_play_state_style_value = coordinated_properties.get(PropertyID::AnimationPlayState).value()[i];
        auto animation_delay_style_value = coordinated_properties.get(PropertyID::AnimationDelay).value()[i];
        auto animation_fill_mode_style_value = coordinated_properties.get(PropertyID::AnimationFillMode).value()[i];
        auto animation_composition_style_value = coordinated_properties.get(PropertyID::AnimationComposition).value()[i];
        auto animation_timeline_style_value = coordinated_properties.get(PropertyID::AnimationTimeline).value()[i];

        // https://drafts.csswg.org/css-animations-2/#animation-duration
        auto duration = [&] -> Variant<double, Utf16String> {
            // auto
            if (animation_duration_style_value->to_keyword() == Keyword::Auto) {
                // Preserve auto until the animation effect is associated with its timeline. Time-driven animations
                // will resolve this to 0s, while scroll-driven animations fill the progress-based timeline.
                return "auto"_utf16;
            }

            // <time [0s,∞]>

            // FIXME: For scroll-driven animations, treated as auto.

            // For time-driven animations, specifies the length of time that an animation takes to complete one cycle.
            // A negative <time> is invalid.
            return Time::from_style_value(animation_duration_style_value, {}).to_milliseconds();
        }();

        auto timing_function = EasingFunction::from_style_value(animation_timing_function_style_value);

        auto iteration_count = [&] {
            if (animation_iteration_count_style_value->to_keyword() == Keyword::Infinite)
                return AK::Infinity<double>;

            return number_from_style_value(animation_iteration_count_style_value, {});
        }();

        auto direction = keyword_to_animation_direction(animation_direction_style_value->to_keyword()).value();
        auto play_state = keyword_to_animation_play_state(animation_play_state_style_value->to_keyword()).value();
        auto delay = Time::from_style_value(animation_delay_style_value, {}).to_milliseconds();
        auto fill_mode = keyword_to_animation_fill_mode(animation_fill_mode_style_value->to_keyword()).value();
        auto composition = keyword_to_animation_composition(animation_composition_style_value->to_keyword()).value();
        auto const& name = string_from_style_value(animation_name_style_value);

        // https://drafts.csswg.org/css-animations-2/#animation-timeline
        auto const& timeline = [&]() -> GC::Ptr<Animations::AnimationTimeline> {
            // auto
            // The animation’s timeline is a DocumentTimeline, more specifically the default document timeline.
            if (animation_timeline_style_value->to_keyword() == Keyword::Auto)
                return abstract_element.document().timeline();

            // none
            // The animation is not associated with a timeline.
            if (animation_timeline_style_value->to_keyword() == Keyword::None)
                return nullptr;

            // <dashed-ident>
            // FIXME: If a named scroll progress timeline or view progress timeline is in scope on this element, use the
            //        referenced timeline as defined in Scroll-driven Animations §  Declaring a Named Timeline’s Scope:
            //        the timeline-scope property. Otherwise the animation is not associated with a timeline.

            // <scroll()>
            // Use the scroll progress timeline indicated by the given scroll() function. See Scroll-driven Animations
            // § 2.2.1 The scroll() notation.
            if (animation_timeline_style_value->is_function() && animation_timeline_style_value->as_function().name() == "scroll"_utf16_fly_string) {
                auto const& arguments = animation_timeline_style_value->as_function().value()->as_tuple().tuple();

                auto const& scroller = arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]
                    ? keyword_to_scroller(arguments[TupleStyleValue::Indices::ScrollFunction::Scroller]->to_keyword()).value()
                    : Scroller::Nearest;

                auto const& axis = arguments[TupleStyleValue::Indices::ScrollFunction::Axis]
                    ? Animations::css_axis_to_bindings_scroll_axis(keyword_to_axis(arguments[TupleStyleValue::Indices::ScrollFunction::Axis]->to_keyword()).value())
                    : Bindings::ScrollAxis::Block;

                Animations::ScrollTimeline::AnonymousSource source {
                    .scroller = scroller,
                    .target = abstract_element,
                };

                return Animations::ScrollTimeline::create(abstract_element.element().realm(), abstract_element.document(), source, axis);
            }

            //<view()>
            // FIXME: Use the view progress timeline indicated by the given view() function. See Scroll-driven
            //        Animations § 3.3.1 The view() notation.

            // FIXME: We fall back to document timeline for now as though we don't support the `animation-timeline`
            //        property at all
            return abstract_element.document().timeline();
        }();

        animations.append(AnimationProperties {
            .duration = duration,
            .timing_function = timing_function,
            .iteration_count = iteration_count,
            .direction = direction,
            .play_state = play_state,
            .delay = delay,
            .fill_mode = fill_mode,
            .composition = composition,
            .name = name,
            .timeline = timeline,
        });
    }

    return animations;
}

Vector<TransitionProperties> ComputedProperties::transitions() const
{
    auto const& coordinated_properties = assemble_coordinated_value_list(
        PropertyID::TransitionProperty,
        { PropertyID::TransitionProperty, PropertyID::TransitionDuration, PropertyID::TransitionTimingFunction, PropertyID::TransitionDelay, PropertyID::TransitionBehavior });

    auto const& property_values = coordinated_properties.get(PropertyID::TransitionProperty).value();
    auto const& duration_values = coordinated_properties.get(PropertyID::TransitionDuration).value();
    auto const& timing_function_values = coordinated_properties.get(PropertyID::TransitionTimingFunction).value();
    auto const& delay_values = coordinated_properties.get(PropertyID::TransitionDelay).value();
    auto const& behavior_values = coordinated_properties.get(PropertyID::TransitionBehavior).value();

    Vector<TransitionProperties> transitions;
    transitions.ensure_capacity(property_values.size());

    for (size_t i = 0; i < property_values.size(); i++) {
        auto properties = [&]() -> Vector<PropertyID> {
            auto const& property_value = property_values[i];

            if (property_value->is_keyword() && property_value->to_keyword() == Keyword::None)
                return {};

            auto maybe_property = property_id_from_string(property_value->as_custom_ident().custom_ident());
            if (!maybe_property.has_value())
                return {};

            Vector<PropertyID> properties;

            auto const append_property_mapping_logical_aliases = [&](PropertyID property_id) {
                if (property_is_logical_alias(property_id))
                    properties.append(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { writing_mode(), direction() }));
                else if (property_id != PropertyID::Custom)
                    properties.append(property_id);
            };

            auto transition_property = maybe_property.release_value();
            if (property_is_shorthand(transition_property)) {
                auto expanded_longhands = expanded_longhands_for_shorthand(transition_property);

                properties.ensure_capacity(expanded_longhands.size());

                for (auto const& prop : expanded_longhands_for_shorthand(transition_property))
                    append_property_mapping_logical_aliases(prop);
            } else {
                append_property_mapping_logical_aliases(transition_property);
            }

            return properties;
        }();

        transitions.append(TransitionProperties {
            .properties = properties,
            .duration = Time::from_style_value(duration_values[i], {}).to_milliseconds(),
            .timing_function = EasingFunction::from_style_value(timing_function_values[i]),
            .delay = Time::from_style_value(delay_values[i], {}).to_milliseconds(),
            .transition_behavior = keyword_to_transition_behavior(behavior_values[i]->to_keyword()).value(),
        });
    }

    return transitions;
}

MaskType ComputedProperties::mask_type() const
{
    auto const& value = property(PropertyID::MaskType);
    return keyword_to_mask_type(value.to_keyword()).release_value();
}

QuotesData ComputedProperties::quotes() const
{
    auto const& value = property(PropertyID::Quotes);
    if (value.is_keyword()) {
        switch (value.to_keyword()) {
        case Keyword::Auto:
            return QuotesData { .type = QuotesData::Type::Auto };
        case Keyword::None:
            return QuotesData { .type = QuotesData::Type::None };
        default:
            break;
        }
    }
    if (value.is_value_list()) {
        auto& value_list = value.as_value_list();
        QuotesData quotes_data { .type = QuotesData::Type::Specified };
        VERIFY(value_list.size() % 2 == 0);
        for (auto i = 0u; i < value_list.size(); i += 2) {
            quotes_data.strings.empend(
                value_list.value_at(i, false)->as_string().string_value(),
                value_list.value_at(i + 1, false)->as_string().string_value());
        }
        return quotes_data;
    }

    return InitialValues::quotes();
}

Vector<CounterData> ComputedProperties::counter_data(PropertyID property_id) const
{
    auto const& value = property(property_id);

    if (value.is_counter_definitions()) {
        auto& counter_definitions = value.as_counter_definitions().counter_definitions();
        Vector<CounterData> result;
        for (auto& counter : counter_definitions) {
            CounterData data {
                .name = counter.name,
                .is_reversed = counter.is_reversed,
                .value = {},
            };

            if (counter.value)
                data.value = AK::clamp_to<i32>(int_from_style_value(*counter.value));

            result.append(move(data));
        }
        return result;
    }

    if (value.to_keyword() == Keyword::None)
        return {};

    dbgln("Unhandled type for {} value: '{}'", string_from_property_id(property_id), value.to_string(SerializationMode::Normal));
    return {};
}

ScrollbarColorData ComputedProperties::scrollbar_color(ColorResolutionContext const& color_resolution_context) const
{
    auto const& value = property(PropertyID::ScrollbarColor);
    if (value.is_keyword() && value.as_keyword().keyword() == Keyword::Auto)
        return InitialValues::scrollbar_color();

    if (value.is_scrollbar_color()) {
        auto& scrollbar_color_value = value.as_scrollbar_color();
        auto thumb_color = scrollbar_color_value.thumb_color()->to_color(color_resolution_context).value();
        auto track_color = scrollbar_color_value.track_color()->to_color(color_resolution_context).value();
        return {
            .thumb_color = thumb_color,
            .track_color = track_color,
            .is_auto = false,
        };
    }

    return {};
}

ScrollbarWidth ComputedProperties::scrollbar_width() const
{
    auto const& value = property(PropertyID::ScrollbarWidth);
    return keyword_to_scrollbar_width(value.to_keyword()).release_value();
}

Resize ComputedProperties::resize() const
{
    auto const& value = property(PropertyID::Resize);
    return keyword_to_resize(value.to_keyword()).release_value();
}

ShapeRendering ComputedProperties::shape_rendering() const
{
    auto const& value = property(PropertyID::ShapeRendering);
    return keyword_to_shape_rendering(value.to_keyword()).release_value();
}

PaintOrderList ComputedProperties::paint_order() const
{
    auto const& value = property(PropertyID::PaintOrder);
    if (value.is_keyword()) {
        auto keyword = value.as_keyword().keyword();
        if (keyword == Keyword::Normal)
            return InitialValues::paint_order();
        auto paint_order_keyword = keyword_to_paint_order(keyword);
        VERIFY(paint_order_keyword.has_value());
        switch (*paint_order_keyword) {
        case PaintOrder::Fill:
            return InitialValues::paint_order();
        case PaintOrder::Stroke:
            return PaintOrderList { PaintOrder::Stroke, PaintOrder::Fill, PaintOrder::Markers };
        case PaintOrder::Markers:
            return PaintOrderList { PaintOrder::Markers, PaintOrder::Fill, PaintOrder::Stroke };
        }
    }

    VERIFY(value.is_value_list());
    auto const& value_list = value.as_value_list();
    // The list must contain 2 values at this point, since the third value is omitted during parsing due to the
    // shortest-serialization principle.
    VERIFY(value_list.size() == 2);
    PaintOrderList paint_order_list {};

    // We use the sum of the keyword values to infer what the missing keyword is. Since each keyword can only appear in
    // the list once, the sum of their values will always be 3.
    auto sum = 0;
    for (auto i = 0; i < 2; i++) {
        auto keyword = value_list.value_at(i, false)->as_keyword().keyword();
        auto paint_order_keyword = keyword_to_paint_order(keyword);
        VERIFY(paint_order_keyword.has_value());
        sum += to_underlying(*paint_order_keyword);
        paint_order_list[i] = *paint_order_keyword;
    }
    VERIFY(sum <= 3);
    paint_order_list[2] = static_cast<PaintOrder>(3 - sum);
    return paint_order_list;
}

WillChange ComputedProperties::will_change() const
{
    auto const& value = property(PropertyID::WillChange);
    if (value.to_keyword() == Keyword::Auto)
        return WillChange::make_auto();

    auto to_will_change_entry = [](StyleValue const& value) -> Optional<WillChange::WillChangeEntry> {
        if (value.is_keyword()) {
            switch (value.as_keyword().keyword()) {
            case Keyword::Contents:
                return WillChange::Type::Contents;
            case Keyword::ScrollPosition:
                return WillChange::Type::ScrollPosition;
            default:
                VERIFY_NOT_REACHED();
            }
        }
        VERIFY(value.is_custom_ident());
        auto custom_ident = value.as_custom_ident().custom_ident();
        auto property_id = property_id_from_string(custom_ident);
        if (!property_id.has_value())
            return {};

        return property_id.release_value();
    };

    auto const& value_list = value.as_value_list();
    Vector<WillChange::WillChangeEntry> will_change_entries;
    for (auto const& style_value : value_list.values()) {
        if (auto entry = to_will_change_entry(*style_value); entry.has_value())
            will_change_entries.append(*entry);
    }

    return WillChange(move(will_change_entries));
}

ValueComparingNonnullRefPtr<Gfx::FontCascadeList const> ComputedProperties::computed_font_list(FontComputer const& font_computer) const
{
    if (!m_cached_computed_font_list) {
        m_cached_computed_font_list = font_computer.compute_font_for_style_values(property(PropertyID::FontFamily), font_size(), font_slope(), font_weight(), font_width(), font_optical_sizing(), font_variation_settings(), font_feature_data());
        VERIFY(!m_cached_computed_font_list->is_empty());
    }

    return *m_cached_computed_font_list;
}

ValueComparingNonnullRefPtr<Gfx::Font const> ComputedProperties::first_available_computed_font(FontComputer const& font_computer) const
{
    if (!m_cached_first_available_computed_font) {
        // https://drafts.csswg.org/css-fonts/#first-available-font
        // First font for which the character U+0020 (space) is not excluded by a unicode-range
        m_cached_first_available_computed_font = computed_font_list(font_computer)->font_for_code_point(' ');
    }

    return *m_cached_first_available_computed_font;
}

MathStyle ComputedProperties::math_style() const
{
    return keyword_to_math_style(property(PropertyID::MathStyle).to_keyword()).value();
}

int ComputedProperties::math_depth() const
{
    return property(PropertyID::MathDepth).as_integer().integer();
}

CSSPixels ComputedProperties::font_size() const
{
    return property(PropertyID::FontSize).as_length().length().absolute_length_to_px();
}

double ComputedProperties::font_weight() const
{
    return property(PropertyID::FontWeight).as_number().number();
}

Percentage ComputedProperties::font_width() const
{
    return property(PropertyID::FontWidth).as_percentage().percentage();
}

int ComputedProperties::font_slope() const
{
    return property(PropertyID::FontStyle).as_font_style().to_font_slope();
}

FontOpticalSizing ComputedProperties::font_optical_sizing() const
{
    auto const& value = property(PropertyID::FontOpticalSizing);
    return keyword_to_font_optical_sizing(value.to_keyword()).release_value();
}

}
