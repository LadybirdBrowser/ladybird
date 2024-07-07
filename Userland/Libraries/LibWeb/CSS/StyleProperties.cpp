/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibCore/DirIterator.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/StyleProperties.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IdentifierStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

bool StyleProperties::is_property_important(CSS::PropertyID property_id) const
{
    return m_property_values[to_underlying(property_id)].style && m_property_values[to_underlying(property_id)].important == Important::Yes;
}

bool StyleProperties::is_property_inherited(CSS::PropertyID property_id) const
{
    return m_property_values[to_underlying(property_id)].style && m_property_values[to_underlying(property_id)].inherited == Inherited::Yes;
}

void StyleProperties::set_property(CSS::PropertyID id, NonnullRefPtr<StyleValue const> value, CSS::CSSStyleDeclaration const* source_declaration, Inherited inherited, Important important)
{
    m_property_values[to_underlying(id)] = StyleAndSourceDeclaration { move(value), source_declaration, important, inherited };
}

void StyleProperties::set_animated_property(CSS::PropertyID id, NonnullRefPtr<StyleValue const> value)
{
    m_animated_property_values.set(id, move(value));
}

void StyleProperties::reset_animated_properties()
{
    m_animated_property_values.clear();
}

NonnullRefPtr<StyleValue const> StyleProperties::property(CSS::PropertyID property_id) const
{
    if (auto animated_value = m_animated_property_values.get(property_id).value_or(nullptr))
        return *animated_value;

    // By the time we call this method, all properties have values assigned.
    return *m_property_values[to_underlying(property_id)].style;
}

RefPtr<StyleValue const> StyleProperties::maybe_null_property(CSS::PropertyID property_id) const
{
    if (auto animated_value = m_animated_property_values.get(property_id).value_or(nullptr))
        return *animated_value;
    return m_property_values[to_underlying(property_id)].style;
}

CSS::CSSStyleDeclaration const* StyleProperties::property_source_declaration(CSS::PropertyID property_id) const
{
    return m_property_values[to_underlying(property_id)].declaration;
}

CSS::Size StyleProperties::size_value(CSS::PropertyID id) const
{
    auto value = property(id);
    if (value->is_identifier()) {
        switch (value->to_identifier()) {
        case ValueID::Auto:
            return CSS::Size::make_auto();
        case ValueID::MinContent:
            return CSS::Size::make_min_content();
        case ValueID::MaxContent:
            return CSS::Size::make_max_content();
        case ValueID::FitContent:
            return CSS::Size::make_fit_content();
        case ValueID::None:
            return CSS::Size::make_none();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    if (value->is_calculated())
        return CSS::Size::make_calculated(const_cast<CalculatedStyleValue&>(value->as_calculated()));

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

    if (value->is_calculated())
        return LengthPercentage { const_cast<CalculatedStyleValue&>(value->as_calculated()) };

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

    if (line_height->is_identifier() && line_height->to_identifier() == ValueID::Normal)
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

    if (line_height->is_calculated()) {
        if (line_height->as_calculated().resolves_to_number()) {
            auto resolved = line_height->as_calculated().resolve_number();
            if (!resolved.has_value()) {
                dbgln("FIXME: Failed to resolve calc() line-height (number): {}", line_height->as_calculated().to_string());
                return CSSPixels::nearest_value_for(m_font_list->first().pixel_metrics().line_spacing());
            }
            return Length(resolved.value(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);
        }

        auto resolved = line_height->as_calculated().resolve_length(Length::ResolutionContext { viewport_rect, font_metrics, root_font_metrics });
        if (!resolved.has_value()) {
            dbgln("FIXME: Failed to resolve calc() line-height: {}", line_height->as_calculated().to_string());
            return CSSPixels::nearest_value_for(m_font_list->first().pixel_metrics().line_spacing());
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

float StyleProperties::resolve_opacity_value(CSS::StyleValue const& value)
{
    float unclamped_opacity = 1.0f;

    if (value.is_number()) {
        unclamped_opacity = value.as_number().number();
    } else if (value.is_calculated()) {
        auto& calculated = value.as_calculated();
        if (calculated.resolves_to_percentage()) {
            auto maybe_percentage = value.as_calculated().resolve_percentage();
            if (maybe_percentage.has_value())
                unclamped_opacity = maybe_percentage->as_fraction();
            else
                dbgln("Unable to resolve calc() as opacity (percentage): {}", value.to_string());
        } else if (calculated.resolves_to_number()) {
            auto maybe_number = const_cast<CalculatedStyleValue&>(value.as_calculated()).resolve_number();
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
    return value_id_to_fill_rule(value->to_identifier());
}

Optional<CSS::ClipRule> StyleProperties::clip_rule() const
{
    auto value = property(CSS::PropertyID::ClipRule);
    return value_id_to_fill_rule(value->to_identifier());
}

Optional<CSS::FlexDirection> StyleProperties::flex_direction() const
{
    auto value = property(CSS::PropertyID::FlexDirection);
    return value_id_to_flex_direction(value->to_identifier());
}

Optional<CSS::FlexWrap> StyleProperties::flex_wrap() const
{
    auto value = property(CSS::PropertyID::FlexWrap);
    return value_id_to_flex_wrap(value->to_identifier());
}

Optional<CSS::FlexBasis> StyleProperties::flex_basis() const
{
    auto value = property(CSS::PropertyID::FlexBasis);

    if (value->is_identifier() && value->to_identifier() == CSS::ValueID::Content)
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
    return value_id_to_image_rendering(value->to_identifier());
}

CSS::Length StyleProperties::border_spacing_horizontal() const
{
    auto value = property(CSS::PropertyID::BorderSpacing);
    if (value->is_length())
        return value->as_length().length();
    auto const& list = value->as_value_list();
    return list.value_at(0, false)->as_length().length();
}

CSS::Length StyleProperties::border_spacing_vertical() const
{
    auto value = property(CSS::PropertyID::BorderSpacing);
    if (value->is_length())
        return value->as_length().length();
    auto const& list = value->as_value_list();
    return list.value_at(1, false)->as_length().length();
}

Optional<CSS::CaptionSide> StyleProperties::caption_side() const
{
    auto value = property(CSS::PropertyID::CaptionSide);
    return value_id_to_caption_side(value->to_identifier());
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
    return value_id_to_justify_content(value->to_identifier());
}

Optional<CSS::JustifyItems> StyleProperties::justify_items() const
{
    auto value = property(CSS::PropertyID::JustifyItems);
    return value_id_to_justify_items(value->to_identifier());
}

Optional<CSS::JustifySelf> StyleProperties::justify_self() const
{
    auto value = property(CSS::PropertyID::JustifySelf);
    return value_id_to_justify_self(value->to_identifier());
}

Vector<CSS::Transformation> StyleProperties::transformations_for_style_value(StyleValue const& value)
{
    if (value.is_identifier() && value.to_identifier() == CSS::ValueID::None)
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
            if (transformation_value->is_calculated()) {
                auto& calculated = transformation_value->as_calculated();
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

static Optional<LengthPercentage> length_percentage_for_style_value(StyleValue const& value)
{
    if (value.is_length())
        return value.as_length().length();
    if (value.is_percentage())
        return value.as_percentage().percentage();
    return {};
}

Optional<CSS::TransformBox> StyleProperties::transform_box() const
{
    auto value = property(CSS::PropertyID::TransformBox);
    return value_id_to_transform_box(value->to_identifier());
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
    return value_id_to_align_content(value->to_identifier());
}

Optional<CSS::AlignItems> StyleProperties::align_items() const
{
    auto value = property(CSS::PropertyID::AlignItems);
    return value_id_to_align_items(value->to_identifier());
}

Optional<CSS::AlignSelf> StyleProperties::align_self() const
{
    auto value = property(CSS::PropertyID::AlignSelf);
    return value_id_to_align_self(value->to_identifier());
}

Optional<CSS::Appearance> StyleProperties::appearance() const
{
    auto value = property(CSS::PropertyID::Appearance);
    auto appearance = value_id_to_appearance(value->to_identifier());
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

CSS::BackdropFilter StyleProperties::backdrop_filter() const
{
    auto value = property(CSS::PropertyID::BackdropFilter);
    if (value->is_filter_value_list())
        return BackdropFilter(value->as_filter_value_list());
    return BackdropFilter::make_none();
}

Optional<CSS::Positioning> StyleProperties::position() const
{
    auto value = property(CSS::PropertyID::Position);
    return value_id_to_positioning(value->to_identifier());
}

bool StyleProperties::operator==(StyleProperties const& other) const
{
    if (m_property_values.size() != other.m_property_values.size())
        return false;

    for (size_t i = 0; i < m_property_values.size(); ++i) {
        auto const& my_style = m_property_values[i];
        auto const& other_style = other.m_property_values[i];
        if (!my_style.style) {
            if (other_style.style)
                return false;
            continue;
        }
        if (!other_style.style)
            return false;
        auto const& my_value = *my_style.style;
        auto const& other_value = *other_style.style;
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
    return value_id_to_text_anchor(value->to_identifier());
}

Optional<CSS::TextAlign> StyleProperties::text_align() const
{
    auto value = property(CSS::PropertyID::TextAlign);
    return value_id_to_text_align(value->to_identifier());
}

Optional<CSS::TextJustify> StyleProperties::text_justify() const
{
    auto value = property(CSS::PropertyID::TextJustify);
    return value_id_to_text_justify(value->to_identifier());
}

Optional<CSS::PointerEvents> StyleProperties::pointer_events() const
{
    auto value = property(CSS::PropertyID::PointerEvents);
    return value_id_to_pointer_events(value->to_identifier());
}

Optional<CSS::WhiteSpace> StyleProperties::white_space() const
{
    auto value = property(CSS::PropertyID::WhiteSpace);
    return value_id_to_white_space(value->to_identifier());
}

Optional<CSS::LineStyle> StyleProperties::line_style(CSS::PropertyID property_id) const
{
    auto value = property(property_id);
    return value_id_to_line_style(value->to_identifier());
}

Optional<CSS::OutlineStyle> StyleProperties::outline_style() const
{
    auto value = property(CSS::PropertyID::OutlineStyle);
    return value_id_to_outline_style(value->to_identifier());
}

Optional<CSS::Float> StyleProperties::float_() const
{
    auto value = property(CSS::PropertyID::Float);
    return value_id_to_float(value->to_identifier());
}

Optional<CSS::Clear> StyleProperties::clear() const
{
    auto value = property(CSS::PropertyID::Clear);
    return value_id_to_clear(value->to_identifier());
}

StyleProperties::ContentDataAndQuoteNestingLevel StyleProperties::content(u32 initial_quote_nesting_level) const
{
    auto value = property(CSS::PropertyID::Content);
    auto quotes_data = quotes();

    auto quote_nesting_level = initial_quote_nesting_level;

    auto get_quote_string = [&](bool open, auto depth) {
        switch (quotes_data.type) {
        case QuotesData::Type::None:
            return String {};
        case QuotesData::Type::Auto:
            // FIXME: "A typographically appropriate used value for quotes is automatically chosen by the UA
            //        based on the content language of the element and/or its parent."
            if (open)
                return depth == 0 ? "“"_string : "‘"_string;
            return depth == 0 ? "”"_string : "’"_string;
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
            } else if (item->is_identifier()) {
                switch (item->to_identifier()) {
                case ValueID::OpenQuote:
                    builder.append(get_quote_string(true, quote_nesting_level++));
                    break;
                case ValueID::CloseQuote:
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    if (quote_nesting_level > 0)
                        builder.append(get_quote_string(false, --quote_nesting_level));
                    break;
                case ValueID::NoOpenQuote:
                    quote_nesting_level++;
                    break;
                case ValueID::NoCloseQuote:
                    // NOTE: See CloseQuote
                    if (quote_nesting_level > 0)
                        quote_nesting_level--;
                    break;
                default:
                    dbgln("`{}` is not supported in `content` (yet?)", item->to_string());
                    break;
                }
            } else {
                // TODO: Implement counters, images, and other things.
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
                } else {
                    // TODO: Implement counters
                }
            }
            content_data.alt_text = MUST(alt_text_builder.to_string());
        }

        return { content_data, quote_nesting_level };
    }

    switch (value->to_identifier()) {
    case ValueID::None:
        return { { ContentData::Type::None }, quote_nesting_level };
    case ValueID::Normal:
        return { { ContentData::Type::Normal }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
}

Optional<CSS::Cursor> StyleProperties::cursor() const
{
    auto value = property(CSS::PropertyID::Cursor);
    return value_id_to_cursor(value->to_identifier());
}

Optional<CSS::Visibility> StyleProperties::visibility() const
{
    auto value = property(CSS::PropertyID::Visibility);
    if (!value->is_identifier())
        return {};
    return value_id_to_visibility(value->to_identifier());
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
            lines.append(value_id_to_text_decoration_line(item->to_identifier()).value());
        }
        return lines;
    }

    if (value->is_identifier() && value->to_identifier() == ValueID::None)
        return {};

    dbgln("FIXME: Unsupported value for text-decoration-line: {}", value->to_string());
    return {};
}

Optional<CSS::TextDecorationStyle> StyleProperties::text_decoration_style() const
{
    auto value = property(CSS::PropertyID::TextDecorationStyle);
    return value_id_to_text_decoration_style(value->to_identifier());
}

Optional<CSS::TextTransform> StyleProperties::text_transform() const
{
    auto value = property(CSS::PropertyID::TextTransform);
    return value_id_to_text_transform(value->to_identifier());
}

Optional<CSS::ListStyleType> StyleProperties::list_style_type() const
{
    auto value = property(CSS::PropertyID::ListStyleType);
    return value_id_to_list_style_type(value->to_identifier());
}

Optional<CSS::ListStylePosition> StyleProperties::list_style_position() const
{
    auto value = property(CSS::PropertyID::ListStylePosition);
    return value_id_to_list_style_position(value->to_identifier());
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
    return value_id_to_overflow(value->to_identifier());
}

Vector<ShadowData> StyleProperties::shadow(PropertyID property_id, Layout::Node const& layout_node) const
{
    auto value = property(property_id);

    auto resolve_to_length = [&layout_node](NonnullRefPtr<StyleValue const> const& value) -> Optional<Length> {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_calculated())
            return value->as_calculated().resolve_length(layout_node);
        return {};
    };

    auto make_shadow_data = [resolve_to_length](ShadowStyleValue const& value) -> Optional<ShadowData> {
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
            value.color(),
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
    return value_id_to_box_sizing(value->to_identifier());
}

Variant<CSS::VerticalAlign, CSS::LengthPercentage> StyleProperties::vertical_align() const
{
    auto value = property(CSS::PropertyID::VerticalAlign);

    if (value->is_identifier())
        return value_id_to_vertical_align(value->to_identifier()).release_value();

    if (value->is_length())
        return CSS::LengthPercentage(value->as_length().length());

    if (value->is_percentage())
        return CSS::LengthPercentage(value->as_percentage().percentage());

    if (value->is_calculated())
        return LengthPercentage { const_cast<CalculatedStyleValue&>(value->as_calculated()) };

    VERIFY_NOT_REACHED();
}

Optional<CSS::FontVariant> StyleProperties::font_variant() const
{
    auto value = property(CSS::PropertyID::FontVariant);
    return value_id_to_font_variant(value->to_identifier());
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
    return value_id_to_border_collapse(value->to_identifier());
}

Vector<Vector<String>> StyleProperties::grid_template_areas() const
{
    auto value = property(CSS::PropertyID::GridTemplateAreas);
    return value->as_grid_template_area().grid_template_area();
}

String StyleProperties::grid_area() const
{
    auto value = property(CSS::PropertyID::GridArea);
    return value->as_string().string_value();
}

Optional<CSS::ObjectFit> StyleProperties::object_fit() const
{
    auto value = property(CSS::PropertyID::ObjectFit);
    return value_id_to_object_fit(value->to_identifier());
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
    return value_id_to_table_layout(value->to_identifier());
}

Optional<CSS::MaskType> StyleProperties::mask_type() const
{
    auto value = property(CSS::PropertyID::MaskType);
    return value_id_to_mask_type(value->to_identifier());
}

Color StyleProperties::stop_color() const
{
    auto value = property(CSS::PropertyID::StopColor);
    if (value->is_identifier()) {
        // Workaround lack of layout node to resolve current color.
        auto& ident = value->as_identifier();
        if (ident.id() == CSS::ValueID::Currentcolor)
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
    m_math_depth = math_depth;
    // Make our children inherit our computed value, not our specified value.
    set_property(PropertyID::MathDepth, MathDepthStyleValue::create_integer(IntegerStyleValue::create(math_depth)));
}

QuotesData StyleProperties::quotes() const
{
    auto value = property(CSS::PropertyID::Quotes);
    if (value->is_identifier()) {
        switch (value->to_identifier()) {
        case ValueID::Auto:
            return QuotesData { .type = QuotesData::Type::Auto };
        case ValueID::None:
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

Optional<CSS::ScrollbarWidth> StyleProperties::scrollbar_width() const
{
    auto value = property(CSS::PropertyID::ScrollbarWidth);
    return value_id_to_scrollbar_width(value->to_identifier());
}

}
