/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibCore/DirIterator.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RotationStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

NonnullRefPtr<StyleProperties::Data> StyleProperties::Data::clone() const
{
    auto clone = adopt_ref(*new StyleProperties::Data);
    clone->m_animation_name_source = m_animation_name_source;
    clone->m_transition_property_source = m_transition_property_source;
    clone->m_property_values = m_property_values;
    clone->m_property_important = m_property_important;
    clone->m_property_inherited = m_property_inherited;
    clone->m_animated_property_values = m_animated_property_values;
    clone->m_math_depth = m_math_depth;
    clone->m_font_list = m_font_list;
    clone->m_line_height = m_line_height;
    return clone;
}

bool StyleProperties::is_property_important(CSS::PropertyID property_id) const
{
    size_t n = to_underlying(property_id);
    return m_data->m_property_important[n / 8] & (1 << (n % 8));
}

void StyleProperties::set_property_important(CSS::PropertyID property_id, Important important)
{
    size_t n = to_underlying(property_id);
    if (important == Important::Yes)
        m_data->m_property_important[n / 8] |= (1 << (n % 8));
    else
        m_data->m_property_important[n / 8] &= ~(1 << (n % 8));
}

bool StyleProperties::is_property_inherited(CSS::PropertyID property_id) const
{
    size_t n = to_underlying(property_id);
    return m_data->m_property_inherited[n / 8] & (1 << (n % 8));
}

void StyleProperties::set_property_inherited(CSS::PropertyID property_id, Inherited inherited)
{
    size_t n = to_underlying(property_id);
    if (inherited == Inherited::Yes)
        m_data->m_property_inherited[n / 8] |= (1 << (n % 8));
    else
        m_data->m_property_inherited[n / 8] &= ~(1 << (n % 8));
}

void StyleProperties::set_property(CSS::PropertyID id, NonnullRefPtr<CSSStyleValue const> value, Inherited inherited, Important important)
{
    m_data->m_property_values[to_underlying(id)] = move(value);
    set_property_important(id, important);
    set_property_inherited(id, inherited);
}

void StyleProperties::revert_property(CSS::PropertyID id, StyleProperties const& style_for_revert)
{
    m_data->m_property_values[to_underlying(id)] = style_for_revert.m_data->m_property_values[to_underlying(id)];
    set_property_important(id, style_for_revert.is_property_important(id) ? Important::Yes : Important::No);
    set_property_inherited(id, style_for_revert.is_property_inherited(id) ? Inherited::Yes : Inherited::No);
}

void StyleProperties::set_animated_property(CSS::PropertyID id, NonnullRefPtr<CSSStyleValue const> value)
{
    m_data->m_animated_property_values.set(id, move(value));
}

void StyleProperties::reset_animated_properties()
{
    m_data->m_animated_property_values.clear();
}

NonnullRefPtr<CSSStyleValue const> StyleProperties::property(CSS::PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    if (return_animated_value == WithAnimationsApplied::Yes) {
        if (auto animated_value = m_data->m_animated_property_values.get(property_id).value_or(nullptr))
            return *animated_value;
    }

    // By the time we call this method, all properties have values assigned.
    return *m_data->m_property_values[to_underlying(property_id)];
}

RefPtr<CSSStyleValue const> StyleProperties::maybe_null_property(CSS::PropertyID property_id) const
{
    if (auto animated_value = m_data->m_animated_property_values.get(property_id).value_or(nullptr))
        return *animated_value;
    return m_data->m_property_values[to_underlying(property_id)];
}

CSS::Size StyleProperties::size_value(CSS::PropertyID id) const
{
    auto value = property(id);
    if (value->is_keyword()) {
        switch (value->to_keyword()) {
        case Keyword::Auto:
            return CSS::Size::make_auto();
        case Keyword::MinContent:
            return CSS::Size::make_min_content();
        case Keyword::MaxContent:
            return CSS::Size::make_max_content();
        case Keyword::FitContent:
            return CSS::Size::make_fit_content();
        case Keyword::None:
            return CSS::Size::make_none();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (value->is_math())
        return CSS::Size::make_calculated(const_cast<CSSMathValue&>(value->as_math()));

    if (value->is_percentage())
        return CSS::Size::make_percentage(value->as_percentage().percentage());

    if (value->is_length()) {
        auto length = value->as_length().length();
        if (length.is_auto())
            return CSS::Size::make_auto();
        return CSS::Size::make_length(length);
    }

    // FIXME: Support `fit-content(<length>)`
    dbgln("FIXME: Unsupported size value: `{}`, treating as `auto`", value->to_string());
    return CSS::Size::make_auto();
}

LengthPercentage StyleProperties::length_percentage_or_fallback(CSS::PropertyID id, LengthPercentage const& fallback) const
{
    return length_percentage(id).value_or(fallback);
}

Optional<LengthPercentage> StyleProperties::length_percentage(CSS::PropertyID id) const
{
    auto value = property(id);

    if (value->is_math())
        return LengthPercentage { const_cast<CSSMathValue&>(value->as_math()) };

    if (value->is_percentage())
        return value->as_percentage().percentage();

    if (value->is_length())
        return value->as_length().length();

    if (value->has_auto())
        return LengthPercentage { Length::make_auto() };

    return {};
}

LengthBox StyleProperties::length_box(CSS::PropertyID left_id, CSS::PropertyID top_id, CSS::PropertyID right_id, CSS::PropertyID bottom_id, const CSS::Length& default_value) const
{
    LengthBox box;
    box.left() = length_percentage_or_fallback(left_id, default_value);
    box.top() = length_percentage_or_fallback(top_id, default_value);
    box.right() = length_percentage_or_fallback(right_id, default_value);
    box.bottom() = length_percentage_or_fallback(bottom_id, default_value);
    return box;
}

Color StyleProperties::color_or_fallback(CSS::PropertyID id, Layout::NodeWithStyle const& node, Color fallback) const
{
    auto value = property(id);
    if (!value->has_color())
        return fallback;
    return value->to_color(node);
}

NonnullRefPtr<Gfx::Font const> StyleProperties::font_fallback(bool monospace, bool bold)
{
    if (monospace && bold)
        return Platform::FontPlugin::the().default_fixed_width_font().bold_variant();

    if (monospace)
        return Platform::FontPlugin::the().default_fixed_width_font();

    if (bold)
        return Platform::FontPlugin::the().default_font().bold_variant();

    return Platform::FontPlugin::the().default_font();
}

CSSPixels StyleProperties::compute_line_height(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    auto line_height = property(CSS::PropertyID::LineHeight);

    if (line_height->is_keyword() && line_height->to_keyword() == Keyword::Normal)
        return font_metrics.line_height;

    if (line_height->is_length()) {
        auto line_height_length = line_height->as_length().length();
        if (!line_height_length.is_auto())
            return line_height_length.to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    if (line_height->is_number())
        return Length(line_height->as_number().number(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);

    if (line_height->is_percentage()) {
        // Percentages are relative to 1em. https://www.w3.org/TR/css-inline-3/#valdef-line-height-percentage
        auto& percentage = line_height->as_percentage().percentage();
        return Length(percentage.as_fraction(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    if (line_height->is_math()) {
        if (line_height->as_math().resolves_to_number()) {
            auto resolved = line_height->as_math().resolve_number();
            if (!resolved.has_value()) {
                dbgln("FIXME: Failed to resolve calc() line-height (number): {}", line_height->as_math().to_string());
                return CSSPixels::nearest_value_for(m_data->m_font_list->first().pixel_metrics().line_spacing());
            }
            return Length(resolved.value(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);
        }

        auto resolved = line_height->as_math().resolve_length(Length::ResolutionContext { viewport_rect, font_metrics, root_font_metrics });
        if (!resolved.has_value()) {
            dbgln("FIXME: Failed to resolve calc() line-height: {}", line_height->as_math().to_string());
            return CSSPixels::nearest_value_for(m_data->m_font_list->first().pixel_metrics().line_spacing());
        }
        return resolved->to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    return font_metrics.line_height;
}

Optional<int> StyleProperties::z_index() const
{
    auto value = property(CSS::PropertyID::ZIndex);
    if (value->has_auto())
        return {};
    if (value->is_integer()) {
        // Clamp z-index to the range of a signed 32-bit integer for consistency with other engines.
        auto integer = value->as_integer().integer();
        if (integer >= NumericLimits<int>::max())
            return NumericLimits<int>::max();
        if (integer <= NumericLimits<int>::min())
            return NumericLimits<int>::min();
        return static_cast<int>(integer);
    }
    return {};
}

float StyleProperties::resolve_opacity_value(CSSStyleValue const& value)
{
    float unclamped_opacity = 1.0f;

    if (value.is_number()) {
        unclamped_opacity = value.as_number().number();
    } else if (value.is_math()) {
        auto& calculated = value.as_math();
        if (calculated.resolves_to_percentage()) {
            auto maybe_percentage = value.as_math().resolve_percentage();
            if (maybe_percentage.has_value())
                unclamped_opacity = maybe_percentage->as_fraction();
            else
                dbgln("Unable to resolve calc() as opacity (percentage): {}", value.to_string());
        } else if (calculated.resolves_to_number()) {
            auto maybe_number = const_cast<CSSMathValue&>(value.as_math()).resolve_number();
            if (maybe_number.has_value())
                unclamped_opacity = maybe_number.value();
            else
                dbgln("Unable to resolve calc() as opacity (number): {}", value.to_string());
        }
    } else if (value.is_percentage()) {
        unclamped_opacity = value.as_percentage().percentage().as_fraction();
    }

    return clamp(unclamped_opacity, 0.0f, 1.0f);
}

float StyleProperties::opacity() const
{
    auto value = property(CSS::PropertyID::Opacity);
    return resolve_opacity_value(*value);
}

float StyleProperties::fill_opacity() const
{
    auto value = property(CSS::PropertyID::FillOpacity);
    return resolve_opacity_value(*value);
}

Optional<CSS::StrokeLinecap> StyleProperties::stroke_linecap() const
{
    auto value = property(CSS::PropertyID::StrokeLinecap);
    return keyword_to_stroke_linecap(value->to_keyword());
}

float StyleProperties::stroke_opacity() const
{
    auto value = property(CSS::PropertyID::StrokeOpacity);
    return resolve_opacity_value(*value);
}

float StyleProperties::stop_opacity() const
{
    auto value = property(CSS::PropertyID::StopOpacity);
    return resolve_opacity_value(*value);
}

Optional<CSS::FillRule> StyleProperties::fill_rule() const
{
    auto value = property(CSS::PropertyID::FillRule);
    return keyword_to_fill_rule(value->to_keyword());
}

Optional<CSS::ClipRule> StyleProperties::clip_rule() const
{
    auto value = property(CSS::PropertyID::ClipRule);
    return keyword_to_fill_rule(value->to_keyword());
}

Optional<CSS::FlexDirection> StyleProperties::flex_direction() const
{
    auto value = property(CSS::PropertyID::FlexDirection);
    return keyword_to_flex_direction(value->to_keyword());
}

Optional<CSS::FlexWrap> StyleProperties::flex_wrap() const
{
    auto value = property(CSS::PropertyID::FlexWrap);
    return keyword_to_flex_wrap(value->to_keyword());
}

Optional<CSS::FlexBasis> StyleProperties::flex_basis() const
{
    auto value = property(CSS::PropertyID::FlexBasis);

    if (value->is_keyword() && value->to_keyword() == CSS::Keyword::Content)
        return CSS::FlexBasisContent {};

    return size_value(CSS::PropertyID::FlexBasis);
}

float StyleProperties::flex_grow() const
{
    auto value = property(CSS::PropertyID::FlexGrow);
    if (!value->is_number())
        return 0;
    return value->as_number().number();
}

float StyleProperties::flex_shrink() const
{
    auto value = property(CSS::PropertyID::FlexShrink);
    if (!value->is_number())
        return 1;
    return value->as_number().number();
}

int StyleProperties::order() const
{
    auto value = property(CSS::PropertyID::Order);
    if (!value->is_integer())
        return 0;
    return value->as_integer().integer();
}

Optional<CSS::ImageRendering> StyleProperties::image_rendering() const
{
    auto value = property(CSS::PropertyID::ImageRendering);
    return keyword_to_image_rendering(value->to_keyword());
}

CSS::Length StyleProperties::border_spacing_horizontal(Layout::Node const& layout_node) const
{
    auto value = property(CSS::PropertyID::BorderSpacing);
    if (value->is_length())
        return value->as_length().length();
    if (value->is_math())
        return value->as_math().resolve_length(layout_node).value_or(CSS::Length(0, CSS::Length::Type::Px));
    auto const& list = value->as_value_list();
    return list.value_at(0, false)->as_length().length();
}

CSS::Length StyleProperties::border_spacing_vertical(Layout::Node const& layout_node) const
{
    auto value = property(CSS::PropertyID::BorderSpacing);
    if (value->is_length())
        return value->as_length().length();
    if (value->is_math())
        return value->as_math().resolve_length(layout_node).value_or(CSS::Length(0, CSS::Length::Type::Px));
    auto const& list = value->as_value_list();
    return list.value_at(1, false)->as_length().length();
}

Optional<CSS::CaptionSide> StyleProperties::caption_side() const
{
    auto value = property(CSS::PropertyID::CaptionSide);
    return keyword_to_caption_side(value->to_keyword());
}

CSS::Clip StyleProperties::clip() const
{
    auto value = property(CSS::PropertyID::Clip);
    if (!value->is_rect())
        return CSS::Clip::make_auto();
    return CSS::Clip(value->as_rect().rect());
}

Optional<CSS::JustifyContent> StyleProperties::justify_content() const
{
    auto value = property(CSS::PropertyID::JustifyContent);
    return keyword_to_justify_content(value->to_keyword());
}

Optional<CSS::JustifyItems> StyleProperties::justify_items() const
{
    auto value = property(CSS::PropertyID::JustifyItems);
    return keyword_to_justify_items(value->to_keyword());
}

Optional<CSS::JustifySelf> StyleProperties::justify_self() const
{
    auto value = property(CSS::PropertyID::JustifySelf);
    return keyword_to_justify_self(value->to_keyword());
}

Vector<CSS::Transformation> StyleProperties::transformations_for_style_value(CSSStyleValue const& value)
{
    if (value.is_keyword() && value.to_keyword() == CSS::Keyword::None)
        return {};

    if (!value.is_value_list())
        return {};

    auto& list = value.as_value_list();

    Vector<CSS::Transformation> transformations;

    for (auto& it : list.values()) {
        if (!it->is_transformation())
            return {};
        auto& transformation_style_value = it->as_transformation();
        auto function = transformation_style_value.transform_function();
        auto function_metadata = transform_function_metadata(function);
        Vector<TransformValue> values;
        size_t argument_index = 0;
        for (auto& transformation_value : transformation_style_value.values()) {
            if (transformation_value->is_math()) {
                auto& calculated = transformation_value->as_math();
                if (calculated.resolves_to_length_percentage()) {
                    values.append(CSS::LengthPercentage { calculated });
                } else if (calculated.resolves_to_percentage()) {
                    // FIXME: Maybe transform this for loop to always check the metadata for the correct types
                    if (function_metadata.parameters[argument_index].type == TransformFunctionParameterType::NumberPercentage) {
                        values.append(NumberPercentage { calculated.resolve_percentage().value() });
                    } else {
                        values.append(LengthPercentage { calculated.resolve_percentage().value() });
                    }
                } else if (calculated.resolves_to_number()) {
                    values.append({ Number(Number::Type::Number, calculated.resolve_number().value()) });
                } else if (calculated.resolves_to_angle()) {
                    values.append({ calculated.resolve_angle().value() });
                } else {
                    dbgln("FIXME: Unsupported calc value in transform! {}", calculated.to_string());
                }
            } else if (transformation_value->is_length()) {
                values.append({ transformation_value->as_length().length() });
            } else if (transformation_value->is_percentage()) {
                if (function_metadata.parameters[argument_index].type == TransformFunctionParameterType::NumberPercentage) {
                    values.append(NumberPercentage { transformation_value->as_percentage().percentage() });
                } else {
                    values.append(LengthPercentage { transformation_value->as_percentage().percentage() });
                }
            } else if (transformation_value->is_number()) {
                values.append({ Number(Number::Type::Number, transformation_value->as_number().number()) });
            } else if (transformation_value->is_angle()) {
                values.append({ transformation_value->as_angle().angle() });
            } else {
                dbgln("FIXME: Unsupported value in transform! {}", transformation_value->to_string());
            }
            argument_index++;
        }
        transformations.empend(function, move(values));
    }
    return transformations;
}

Vector<CSS::Transformation> StyleProperties::transformations() const
{
    return transformations_for_style_value(property(CSS::PropertyID::Transform));
}

Optional<CSS::Transformation> StyleProperties::rotate(Layout::Node const& layout_node) const
{
    auto value = property(CSS::PropertyID::Rotate);
    if (!value->is_rotation())
        return {};
    auto& rotation = value->as_rotation();

    auto resolve_angle = [&layout_node](CSSStyleValue const& value) -> Optional<Angle> {
        if (value.is_angle())
            return value.as_angle().angle();
        if (value.is_math() && value.as_math().resolves_to_angle())
            return value.as_math().resolve_angle(layout_node);
        return {};
    };

    auto resolve_number = [&](CSSStyleValue const& value) -> Optional<double> {
        if (value.is_number())
            return value.as_number().number();
        if (value.is_math() && value.as_math().resolves_to_number())
            return value.as_math().resolve_number();
        return {};
    };

    auto x = resolve_number(rotation.rotation_x()).value_or(0);
    auto y = resolve_number(rotation.rotation_y()).value_or(0);
    auto z = resolve_number(rotation.rotation_z()).value_or(0);
    auto angle = resolve_angle(rotation.angle()).value_or(Angle::make_degrees(0));

    Vector<TransformValue> values;
    values.append({ Number(Number::Type::Number, x) });
    values.append({ Number(Number::Type::Number, y) });
    values.append({ Number(Number::Type::Number, z) });
    values.append({ angle });

    return CSS::Transformation(CSS::TransformFunction::Rotate3d, move(values));
}

static Optional<LengthPercentage> length_percentage_for_style_value(CSSStyleValue const& value)
{
    if (value.is_length())
        return value.as_length().length();
    if (value.is_percentage())
        return value.as_percentage().percentage();
    if (value.is_math())
        return LengthPercentage { const_cast<CSSMathValue&>(value.as_math()) };
    return {};
}

Optional<CSS::TransformBox> StyleProperties::transform_box() const
{
    auto value = property(CSS::PropertyID::TransformBox);
    return keyword_to_transform_box(value->to_keyword());
}

CSS::TransformOrigin StyleProperties::transform_origin() const
{
    auto value = property(CSS::PropertyID::TransformOrigin);
    if (!value->is_value_list() || value->as_value_list().size() != 2)
        return {};
    auto const& list = value->as_value_list();
    auto x_value = length_percentage_for_style_value(list.values()[0]);
    auto y_value = length_percentage_for_style_value(list.values()[1]);
    if (!x_value.has_value() || !y_value.has_value()) {
        return {};
    }
    return { x_value.value(), y_value.value() };
}

Optional<Color> StyleProperties::accent_color(Layout::NodeWithStyle const& node) const
{
    auto value = property(CSS::PropertyID::AccentColor);
    if (value->has_color())
        return value->to_color(node);
    return {};
}

Optional<CSS::AlignContent> StyleProperties::align_content() const
{
    auto value = property(CSS::PropertyID::AlignContent);
    return keyword_to_align_content(value->to_keyword());
}

Optional<CSS::AlignItems> StyleProperties::align_items() const
{
    auto value = property(CSS::PropertyID::AlignItems);
    return keyword_to_align_items(value->to_keyword());
}

Optional<CSS::AlignSelf> StyleProperties::align_self() const
{
    auto value = property(CSS::PropertyID::AlignSelf);
    return keyword_to_align_self(value->to_keyword());
}

Optional<CSS::Appearance> StyleProperties::appearance() const
{
    auto value = property(CSS::PropertyID::Appearance);
    auto appearance = keyword_to_appearance(value->to_keyword());
    if (appearance.has_value()) {
        switch (*appearance) {
        // Note: All these compatibility values can be treated as 'auto'
        case CSS::Appearance::Textfield:
        case CSS::Appearance::MenulistButton:
        case CSS::Appearance::Searchfield:
        case CSS::Appearance::Textarea:
        case CSS::Appearance::PushButton:
        case CSS::Appearance::SliderHorizontal:
        case CSS::Appearance::Checkbox:
        case CSS::Appearance::Radio:
        case CSS::Appearance::SquareButton:
        case CSS::Appearance::Menulist:
        case CSS::Appearance::Listbox:
        case CSS::Appearance::Meter:
        case CSS::Appearance::ProgressBar:
        case CSS::Appearance::Button:
            appearance = CSS::Appearance::Auto;
            break;
        default:
            break;
        }
    }
    return appearance;
}

CSS::Filter StyleProperties::backdrop_filter() const
{
    auto value = property(CSS::PropertyID::BackdropFilter);
    if (value->is_filter_value_list())
        return Filter(value->as_filter_value_list());
    return Filter::make_none();
}

CSS::Filter StyleProperties::filter() const
{
    auto value = property(CSS::PropertyID::Filter);
    if (value->is_filter_value_list())
        return Filter(value->as_filter_value_list());
    return Filter::make_none();
}

Optional<CSS::Positioning> StyleProperties::position() const
{
    auto value = property(CSS::PropertyID::Position);
    return keyword_to_positioning(value->to_keyword());
}

bool StyleProperties::operator==(StyleProperties const& other) const
{
    if (m_data->m_property_values.size() != other.m_data->m_property_values.size())
        return false;

    for (size_t i = 0; i < m_data->m_property_values.size(); ++i) {
        auto const& my_style = m_data->m_property_values[i];
        auto const& other_style = other.m_data->m_property_values[i];
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

Optional<CSS::TextAnchor> StyleProperties::text_anchor() const
{
    auto value = property(CSS::PropertyID::TextAnchor);
    return keyword_to_text_anchor(value->to_keyword());
}

Optional<CSS::TextAlign> StyleProperties::text_align() const
{
    auto value = property(CSS::PropertyID::TextAlign);
    return keyword_to_text_align(value->to_keyword());
}

Optional<CSS::TextJustify> StyleProperties::text_justify() const
{
    auto value = property(CSS::PropertyID::TextJustify);
    return keyword_to_text_justify(value->to_keyword());
}

Optional<CSS::TextOverflow> StyleProperties::text_overflow() const
{
    auto value = property(CSS::PropertyID::TextOverflow);
    return keyword_to_text_overflow(value->to_keyword());
}

Optional<CSS::PointerEvents> StyleProperties::pointer_events() const
{
    auto value = property(CSS::PropertyID::PointerEvents);
    return keyword_to_pointer_events(value->to_keyword());
}

Variant<LengthOrCalculated, NumberOrCalculated> StyleProperties::tab_size() const
{
    auto value = property(CSS::PropertyID::TabSize);
    if (value->is_math()) {
        auto const& math_value = value->as_math();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
        if (math_value.resolves_to_number()) {
            return NumberOrCalculated { math_value };
        }
    }

    if (value->is_length())
        return LengthOrCalculated { value->as_length().length() };

    return NumberOrCalculated { value->as_number().number() };
}

Optional<CSS::WordBreak> StyleProperties::word_break() const
{
    auto value = property(CSS::PropertyID::WordBreak);
    return keyword_to_word_break(value->to_keyword());
}

Optional<CSS::LengthOrCalculated> StyleProperties::word_spacing() const
{
    auto value = property(CSS::PropertyID::WordSpacing);
    if (value->is_math()) {
        auto& math_value = value->as_math();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
    }

    if (value->is_length())
        return LengthOrCalculated { value->as_length().length() };

    return {};
}

Optional<CSS::WhiteSpace> StyleProperties::white_space() const
{
    auto value = property(CSS::PropertyID::WhiteSpace);
    return keyword_to_white_space(value->to_keyword());
}

Optional<LengthOrCalculated> StyleProperties::letter_spacing() const
{
    auto value = property(CSS::PropertyID::LetterSpacing);
    if (value->is_math()) {
        auto const& math_value = value->as_math();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
    }

    if (value->is_length())
        return LengthOrCalculated { value->as_length().length() };

    return {};
}

Optional<CSS::LineStyle> StyleProperties::line_style(CSS::PropertyID property_id) const
{
    auto value = property(property_id);
    return keyword_to_line_style(value->to_keyword());
}

Optional<CSS::OutlineStyle> StyleProperties::outline_style() const
{
    auto value = property(CSS::PropertyID::OutlineStyle);
    return keyword_to_outline_style(value->to_keyword());
}

Optional<CSS::Float> StyleProperties::float_() const
{
    auto value = property(CSS::PropertyID::Float);
    return keyword_to_float(value->to_keyword());
}

Optional<CSS::Clear> StyleProperties::clear() const
{
    auto value = property(CSS::PropertyID::Clear);
    return keyword_to_clear(value->to_keyword());
}

Optional<CSS::ColumnSpan> StyleProperties::column_span() const
{
    auto value = property(CSS::PropertyID::ColumnSpan);
    return keyword_to_column_span(value->to_keyword());
}

StyleProperties::ContentDataAndQuoteNestingLevel StyleProperties::content(DOM::Element& element, u32 initial_quote_nesting_level) const
{
    auto value = property(CSS::PropertyID::Content);
    auto quotes_data = quotes();

    auto quote_nesting_level = initial_quote_nesting_level;

    auto get_quote_string = [&](bool open, auto depth) {
        switch (quotes_data.type) {
        case QuotesData::Type::None:
            return FlyString {};
        case QuotesData::Type::Auto:
            // FIXME: "A typographically appropriate used value for quotes is automatically chosen by the UA
            //        based on the content language of the element and/or its parent."
            if (open)
                return depth == 0 ? "“"_fly_string : "‘"_fly_string;
            return depth == 0 ? "”"_fly_string : "’"_fly_string;
        case QuotesData::Type::Specified:
            // If the depth is greater than the number of pairs, the last pair is repeated.
            auto& level = quotes_data.strings[min(depth, quotes_data.strings.size() - 1)];
            return open ? level[0] : level[1];
        }
        VERIFY_NOT_REACHED();
    };

    if (value->is_content()) {
        auto& content_style_value = value->as_content();

        CSS::ContentData content_data;

        // FIXME: The content is a list of things: strings, identifiers or functions that return strings, and images.
        //        So it can't always be represented as a single String, but may have to be multiple boxes.
        //        For now, we'll just assume strings since that is easiest.
        StringBuilder builder;
        for (auto const& item : content_style_value.content().values()) {
            if (item->is_string()) {
                builder.append(item->as_string().string_value());
            } else if (item->is_keyword()) {
                switch (item->to_keyword()) {
                case Keyword::OpenQuote:
                    builder.append(get_quote_string(true, quote_nesting_level++));
                    break;
                case Keyword::CloseQuote:
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    if (quote_nesting_level > 0)
                        builder.append(get_quote_string(false, --quote_nesting_level));
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
                    dbgln("`{}` is not supported in `content` (yet?)", item->to_string());
                    break;
                }
            } else if (item->is_counter()) {
                builder.append(item->as_counter().resolve(element));
            } else {
                // TODO: Implement images, and other things.
                dbgln("`{}` is not supported in `content` (yet?)", item->to_string());
            }
        }
        content_data.type = ContentData::Type::String;
        content_data.data = MUST(builder.to_string());

        if (content_style_value.has_alt_text()) {
            StringBuilder alt_text_builder;
            for (auto const& item : content_style_value.alt_text()->values()) {
                if (item->is_string()) {
                    alt_text_builder.append(item->as_string().string_value());
                } else if (item->is_counter()) {
                    alt_text_builder.append(item->as_counter().resolve(element));
                } else {
                    dbgln("`{}` is not supported in `content` alt-text (yet?)", item->to_string());
                }
            }
            content_data.alt_text = MUST(alt_text_builder.to_string());
        }

        return { content_data, quote_nesting_level };
    }

    switch (value->to_keyword()) {
    case Keyword::None:
        return { { ContentData::Type::None }, quote_nesting_level };
    case Keyword::Normal:
        return { { ContentData::Type::Normal }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
}

Optional<CSS::ContentVisibility> StyleProperties::content_visibility() const
{
    auto value = property(CSS::PropertyID::ContentVisibility);
    return keyword_to_content_visibility(value->to_keyword());
}

Optional<CSS::Cursor> StyleProperties::cursor() const
{
    auto value = property(CSS::PropertyID::Cursor);
    return keyword_to_cursor(value->to_keyword());
}

Optional<CSS::Visibility> StyleProperties::visibility() const
{
    auto value = property(CSS::PropertyID::Visibility);
    if (!value->is_keyword())
        return {};
    return keyword_to_visibility(value->to_keyword());
}

Display StyleProperties::display() const
{
    auto value = property(PropertyID::Display);
    if (value->is_display()) {
        return value->as_display().display();
    }
    return Display::from_short(Display::Short::Inline);
}

Vector<CSS::TextDecorationLine> StyleProperties::text_decoration_line() const
{
    auto value = property(CSS::PropertyID::TextDecorationLine);

    if (value->is_value_list()) {
        Vector<CSS::TextDecorationLine> lines;
        auto& values = value->as_value_list().values();
        for (auto const& item : values) {
            lines.append(keyword_to_text_decoration_line(item->to_keyword()).value());
        }
        return lines;
    }

    if (value->is_keyword() && value->to_keyword() == Keyword::None)
        return {};

    dbgln("FIXME: Unsupported value for text-decoration-line: {}", value->to_string());
    return {};
}

Optional<CSS::TextDecorationStyle> StyleProperties::text_decoration_style() const
{
    auto value = property(CSS::PropertyID::TextDecorationStyle);
    return keyword_to_text_decoration_style(value->to_keyword());
}

Optional<CSS::TextTransform> StyleProperties::text_transform() const
{
    auto value = property(CSS::PropertyID::TextTransform);
    return keyword_to_text_transform(value->to_keyword());
}

Optional<CSS::ListStyleType> StyleProperties::list_style_type() const
{
    auto value = property(CSS::PropertyID::ListStyleType);
    return keyword_to_list_style_type(value->to_keyword());
}

Optional<CSS::ListStylePosition> StyleProperties::list_style_position() const
{
    auto value = property(CSS::PropertyID::ListStylePosition);
    return keyword_to_list_style_position(value->to_keyword());
}

Optional<CSS::Overflow> StyleProperties::overflow_x() const
{
    return overflow(CSS::PropertyID::OverflowX);
}

Optional<CSS::Overflow> StyleProperties::overflow_y() const
{
    return overflow(CSS::PropertyID::OverflowY);
}

Optional<CSS::Overflow> StyleProperties::overflow(CSS::PropertyID property_id) const
{
    auto value = property(property_id);
    return keyword_to_overflow(value->to_keyword());
}

Vector<ShadowData> StyleProperties::shadow(PropertyID property_id, Layout::Node const& layout_node) const
{
    auto value = property(property_id);

    auto resolve_to_length = [&layout_node](NonnullRefPtr<CSSStyleValue const> const& value) -> Optional<Length> {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_math())
            return value->as_math().resolve_length(layout_node);
        return {};
    };

    auto make_shadow_data = [resolve_to_length, &layout_node](ShadowStyleValue const& value) -> Optional<ShadowData> {
        auto maybe_offset_x = resolve_to_length(value.offset_x());
        if (!maybe_offset_x.has_value())
            return {};
        auto maybe_offset_y = resolve_to_length(value.offset_y());
        if (!maybe_offset_y.has_value())
            return {};
        auto maybe_blur_radius = resolve_to_length(value.blur_radius());
        if (!maybe_blur_radius.has_value())
            return {};
        auto maybe_spread_distance = resolve_to_length(value.spread_distance());
        if (!maybe_spread_distance.has_value())
            return {};
        return ShadowData {
            value.color()->to_color(verify_cast<Layout::NodeWithStyle>(layout_node)),
            maybe_offset_x.release_value(),
            maybe_offset_y.release_value(),
            maybe_blur_radius.release_value(),
            maybe_spread_distance.release_value(),
            value.placement()
        };
    };

    if (value->is_value_list()) {
        auto const& value_list = value->as_value_list();

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

    if (value->is_shadow()) {
        auto maybe_shadow_data = make_shadow_data(value->as_shadow());
        if (!maybe_shadow_data.has_value())
            return {};
        return { maybe_shadow_data.release_value() };
    }

    return {};
}

Vector<ShadowData> StyleProperties::box_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::BoxShadow, layout_node);
}

Vector<ShadowData> StyleProperties::text_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::TextShadow, layout_node);
}

Optional<CSS::BoxSizing> StyleProperties::box_sizing() const
{
    auto value = property(CSS::PropertyID::BoxSizing);
    return keyword_to_box_sizing(value->to_keyword());
}

Variant<CSS::VerticalAlign, CSS::LengthPercentage> StyleProperties::vertical_align() const
{
    auto value = property(CSS::PropertyID::VerticalAlign);

    if (value->is_keyword())
        return keyword_to_vertical_align(value->to_keyword()).release_value();

    if (value->is_length())
        return CSS::LengthPercentage(value->as_length().length());

    if (value->is_percentage())
        return CSS::LengthPercentage(value->as_percentage().percentage());

    if (value->is_math())
        return LengthPercentage { const_cast<CSSMathValue&>(value->as_math()) };

    VERIFY_NOT_REACHED();
}

Optional<CSS::FontVariant> StyleProperties::font_variant() const
{
    auto value = property(CSS::PropertyID::FontVariant);
    return keyword_to_font_variant(value->to_keyword());
}

Optional<FlyString> StyleProperties::font_language_override() const
{
    auto value = property(CSS::PropertyID::FontLanguageOverride);
    if (value->is_string())
        return value->as_string().string_value();
    return {};
}

Optional<HashMap<FlyString, IntegerOrCalculated>> StyleProperties::font_feature_settings() const
{
    auto value = property(PropertyID::FontFeatureSettings);

    if (value->is_keyword())
        return {}; // normal

    if (value->is_value_list()) {
        auto const& feature_tags = value->as_value_list().values();
        HashMap<FlyString, IntegerOrCalculated> result;
        result.ensure_capacity(feature_tags.size());
        for (auto const& tag_value : feature_tags) {
            auto const& feature_tag = tag_value->as_open_type_tagged();

            if (feature_tag.value()->is_integer()) {
                result.set(feature_tag.tag(), feature_tag.value()->as_integer().value());
            } else {
                VERIFY(feature_tag.value()->is_math());
                result.set(feature_tag.tag(), IntegerOrCalculated { feature_tag.value()->as_math() });
            }
        }
        return result;
    }

    return {};
}

Optional<HashMap<FlyString, NumberOrCalculated>> StyleProperties::font_variation_settings() const
{
    auto value = property(CSS::PropertyID::FontVariationSettings);

    if (value->is_keyword())
        return {}; // normal

    if (value->is_value_list()) {
        auto const& axis_tags = value->as_value_list().values();
        HashMap<FlyString, NumberOrCalculated> result;
        result.ensure_capacity(axis_tags.size());
        for (auto const& tag_value : axis_tags) {
            auto const& axis_tag = tag_value->as_open_type_tagged();

            if (axis_tag.value()->is_number()) {
                result.set(axis_tag.tag(), axis_tag.value()->as_number().value());
            } else {
                VERIFY(axis_tag.value()->is_math());
                result.set(axis_tag.tag(), NumberOrCalculated { axis_tag.value()->as_math() });
            }
        }
        return result;
    }

    return {};
}

CSS::GridTrackSizeList StyleProperties::grid_auto_columns() const
{
    auto value = property(CSS::PropertyID::GridAutoColumns);
    return value->as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList StyleProperties::grid_auto_rows() const
{
    auto value = property(CSS::PropertyID::GridAutoRows);
    return value->as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList StyleProperties::grid_template_columns() const
{
    auto value = property(CSS::PropertyID::GridTemplateColumns);
    return value->as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList StyleProperties::grid_template_rows() const
{
    auto value = property(CSS::PropertyID::GridTemplateRows);
    return value->as_grid_track_size_list().grid_track_size_list();
}

CSS::GridAutoFlow StyleProperties::grid_auto_flow() const
{
    auto value = property(CSS::PropertyID::GridAutoFlow);
    if (!value->is_grid_auto_flow())
        return CSS::GridAutoFlow {};
    auto& grid_auto_flow_value = value->as_grid_auto_flow();
    return CSS::GridAutoFlow { .row = grid_auto_flow_value.is_row(), .dense = grid_auto_flow_value.is_dense() };
}

CSS::GridTrackPlacement StyleProperties::grid_column_end() const
{
    auto value = property(CSS::PropertyID::GridColumnEnd);
    return value->as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement StyleProperties::grid_column_start() const
{
    auto value = property(CSS::PropertyID::GridColumnStart);
    return value->as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement StyleProperties::grid_row_end() const
{
    auto value = property(CSS::PropertyID::GridRowEnd);
    return value->as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement StyleProperties::grid_row_start() const
{
    auto value = property(CSS::PropertyID::GridRowStart);
    return value->as_grid_track_placement().grid_track_placement();
}

Optional<CSS::BorderCollapse> StyleProperties::border_collapse() const
{
    auto value = property(CSS::PropertyID::BorderCollapse);
    return keyword_to_border_collapse(value->to_keyword());
}

Vector<Vector<String>> StyleProperties::grid_template_areas() const
{
    auto value = property(CSS::PropertyID::GridTemplateAreas);
    return value->as_grid_template_area().grid_template_area();
}

Optional<CSS::ObjectFit> StyleProperties::object_fit() const
{
    auto value = property(CSS::PropertyID::ObjectFit);
    return keyword_to_object_fit(value->to_keyword());
}

CSS::ObjectPosition StyleProperties::object_position() const
{
    auto value = property(CSS::PropertyID::ObjectPosition);
    auto const& position = value->as_position();
    CSS::ObjectPosition object_position;
    auto const& edge_x = position.edge_x();
    auto const& edge_y = position.edge_y();
    if (edge_x->is_edge()) {
        auto const& edge = edge_x->as_edge();
        object_position.edge_x = edge.edge();
        object_position.offset_x = edge.offset();
    }
    if (edge_y->is_edge()) {
        auto const& edge = edge_y->as_edge();
        object_position.edge_y = edge.edge();
        object_position.offset_y = edge.offset();
    }
    return object_position;
}

Optional<CSS::TableLayout> StyleProperties::table_layout() const
{
    auto value = property(CSS::PropertyID::TableLayout);
    return keyword_to_table_layout(value->to_keyword());
}

Optional<CSS::Direction> StyleProperties::direction() const
{
    auto value = property(CSS::PropertyID::Direction);
    return keyword_to_direction(value->to_keyword());
}

Optional<CSS::UnicodeBidi> StyleProperties::unicode_bidi() const
{
    auto value = property(CSS::PropertyID::UnicodeBidi);
    return keyword_to_unicode_bidi(value->to_keyword());
}

Optional<CSS::MaskType> StyleProperties::mask_type() const
{
    auto value = property(CSS::PropertyID::MaskType);
    return keyword_to_mask_type(value->to_keyword());
}

Color StyleProperties::stop_color() const
{
    auto value = property(CSS::PropertyID::StopColor);
    if (value->is_keyword()) {
        // Workaround lack of layout node to resolve current color.
        auto& keyword = value->as_keyword();
        if (keyword.keyword() == CSS::Keyword::Currentcolor)
            value = property(CSS::PropertyID::Color);
    }
    if (value->has_color()) {
        // FIXME: This is used by the SVGStopElement, which does not participate in layout,
        // so can't pass a layout node (so can't resolve some colors, e.g. palette ones)
        return value->to_color({});
    }
    return Color::Black;
}

void StyleProperties::set_math_depth(int math_depth)
{
    m_data->m_math_depth = math_depth;
    // Make our children inherit our computed value, not our specified value.
    set_property(PropertyID::MathDepth, MathDepthStyleValue::create_integer(IntegerStyleValue::create(math_depth)));
}

QuotesData StyleProperties::quotes() const
{
    auto value = property(CSS::PropertyID::Quotes);
    if (value->is_keyword()) {
        switch (value->to_keyword()) {
        case Keyword::Auto:
            return QuotesData { .type = QuotesData::Type::Auto };
        case Keyword::None:
            return QuotesData { .type = QuotesData::Type::None };
        default:
            break;
        }
    }
    if (value->is_value_list()) {
        auto& value_list = value->as_value_list();
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

Vector<CounterData> StyleProperties::counter_data(PropertyID property_id) const
{
    auto value = property(property_id);

    if (value->is_counter_definitions()) {
        auto& counter_definitions = value->as_counter_definitions().counter_definitions();
        Vector<CounterData> result;
        for (auto& counter : counter_definitions) {
            CounterData data {
                .name = counter.name,
                .is_reversed = counter.is_reversed,
                .value = {},
            };
            if (counter.value) {
                if (counter.value->is_integer()) {
                    data.value = AK::clamp_to<i32>(counter.value->as_integer().integer());
                } else if (counter.value->is_math()) {
                    auto maybe_int = counter.value->as_math().resolve_integer();
                    if (maybe_int.has_value())
                        data.value = AK::clamp_to<i32>(*maybe_int);
                } else {
                    dbgln("Unimplemented type for {} integer value: '{}'", string_from_property_id(property_id), counter.value->to_string());
                }
            }
            result.append(move(data));
        }
        return result;
    }

    if (value->to_keyword() == Keyword::None)
        return {};

    dbgln("Unhandled type for {} value: '{}'", string_from_property_id(property_id), value->to_string());
    return {};
}

Optional<CSS::ScrollbarWidth> StyleProperties::scrollbar_width() const
{
    auto value = property(CSS::PropertyID::ScrollbarWidth);
    return keyword_to_scrollbar_width(value->to_keyword());
}

}
