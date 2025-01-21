/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRawPtr.h>
#include <AK/TypeCasts.h>
#include <LibCore/DirIterator.h>
#include <LibGC/CellAllocator.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(ComputedProperties);

ComputedProperties::ComputedProperties() = default;

ComputedProperties::~ComputedProperties() = default;

void ComputedProperties::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation_name_source);
    visitor.visit(m_transition_property_source);
}

bool ComputedProperties::is_property_important(CSS::PropertyID property_id) const
{
    size_t n = to_underlying(property_id);
    return m_property_important[n / 8] & (1 << (n % 8));
}

void ComputedProperties::set_property_important(CSS::PropertyID property_id, Important important)
{
    size_t n = to_underlying(property_id);
    if (important == Important::Yes)
        m_property_important[n / 8] |= (1 << (n % 8));
    else
        m_property_important[n / 8] &= ~(1 << (n % 8));
}

bool ComputedProperties::is_property_inherited(CSS::PropertyID property_id) const
{
    size_t n = to_underlying(property_id);
    return m_property_inherited[n / 8] & (1 << (n % 8));
}

void ComputedProperties::set_property_inherited(CSS::PropertyID property_id, Inherited inherited)
{
    size_t n = to_underlying(property_id);
    if (inherited == Inherited::Yes)
        m_property_inherited[n / 8] |= (1 << (n % 8));
    else
        m_property_inherited[n / 8] &= ~(1 << (n % 8));
}

void ComputedProperties::set_property(CSS::PropertyID id, NonnullRefPtr<CSSStyleValue const> value, Inherited inherited, Important important)
{
    m_property_values[to_underlying(id)] = move(value);
    set_property_important(id, important);
    set_property_inherited(id, inherited);
}

void ComputedProperties::revert_property(CSS::PropertyID id, ComputedProperties const& style_for_revert)
{
    m_property_values[to_underlying(id)] = style_for_revert.m_property_values[to_underlying(id)];
    set_property_important(id, style_for_revert.is_property_important(id) ? Important::Yes : Important::No);
    set_property_inherited(id, style_for_revert.is_property_inherited(id) ? Inherited::Yes : Inherited::No);
}

void ComputedProperties::set_animated_property(CSS::PropertyID id, NonnullRefPtr<CSSStyleValue const> value)
{
    m_animated_property_values.set(id, move(value));
}

void ComputedProperties::reset_animated_properties()
{
    m_animated_property_values.clear();
}

CSSStyleValue const& ComputedProperties::property(CSS::PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    if (return_animated_value == WithAnimationsApplied::Yes) {
        if (auto animated_value = m_animated_property_values.get(property_id); animated_value.has_value())
            return *animated_value.value();
    }

    // By the time we call this method, all properties have values assigned.
    return *m_property_values[to_underlying(property_id)];
}

CSSStyleValue const* ComputedProperties::maybe_null_property(CSS::PropertyID property_id) const
{
    if (auto animated_value = m_animated_property_values.get(property_id); animated_value.has_value())
        return animated_value.value();
    return m_property_values[to_underlying(property_id)];
}

Variant<LengthPercentage, NormalGap> ComputedProperties::gap_value(CSS::PropertyID id) const
{
    auto const& value = property(id);
    if (value.is_keyword()) {
        VERIFY(value.as_keyword().keyword() == CSS::Keyword::Normal);
        return NormalGap {};
    }

    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };

    if (value.is_percentage())
        return LengthPercentage { value.as_percentage().percentage() };

    if (value.is_length())
        return LengthPercentage { value.as_length().length() };

    VERIFY_NOT_REACHED();
}

CSS::Size ComputedProperties::size_value(CSS::PropertyID id) const
{
    auto const& value = property(id);
    if (value.is_keyword()) {
        switch (value.to_keyword()) {
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

    if (value.is_calculated())
        return CSS::Size::make_calculated(value.as_calculated());

    if (value.is_percentage())
        return CSS::Size::make_percentage(value.as_percentage().percentage());

    if (value.is_length()) {
        auto length = value.as_length().length();
        if (length.is_auto())
            return CSS::Size::make_auto();
        return CSS::Size::make_length(length);
    }

    // FIXME: Support `fit-content(<length>)`
    dbgln("FIXME: Unsupported size value: `{}`, treating as `auto`", value.to_string(CSSStyleValue::SerializationMode::Normal));
    return CSS::Size::make_auto();
}

LengthPercentage ComputedProperties::length_percentage_or_fallback(CSS::PropertyID id, LengthPercentage const& fallback) const
{
    return length_percentage(id).value_or(fallback);
}

Optional<LengthPercentage> ComputedProperties::length_percentage(CSS::PropertyID id) const
{
    auto const& value = property(id);

    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };

    if (value.is_percentage())
        return value.as_percentage().percentage();

    if (value.is_length())
        return value.as_length().length();

    if (value.has_auto())
        return LengthPercentage { Length::make_auto() };

    return {};
}

LengthBox ComputedProperties::length_box(CSS::PropertyID left_id, CSS::PropertyID top_id, CSS::PropertyID right_id, CSS::PropertyID bottom_id, const CSS::Length& default_value) const
{
    LengthBox box;
    box.left() = length_percentage_or_fallback(left_id, default_value);
    box.top() = length_percentage_or_fallback(top_id, default_value);
    box.right() = length_percentage_or_fallback(right_id, default_value);
    box.bottom() = length_percentage_or_fallback(bottom_id, default_value);
    return box;
}

Color ComputedProperties::color_or_fallback(CSS::PropertyID id, Layout::NodeWithStyle const& node, Color fallback) const
{
    auto const& value = property(id);
    if (!value.has_color())
        return fallback;
    return value.to_color(node);
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
CSS::PreferredColorScheme ComputedProperties::color_scheme(CSS::PreferredColorScheme preferred_scheme, Optional<Vector<String> const&> document_supported_schemes) const
{
    // To determine the used color scheme of an element:
    auto const& scheme_value = property(CSS::PropertyID::ColorScheme).as_color_scheme();

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if (preferred_scheme != CSS::PreferredColorScheme::Auto && scheme_value.schemes().contains_slow(preferred_color_scheme_to_string(preferred_scheme)))
        return preferred_scheme;

    // 2. Otherwise, if the user has indicated an overriding preference for their chosen color scheme,
    //    and the only keyword is not present in color-scheme for the element,
    //    the user agent must override the color scheme with the user’s preferred color scheme.
    //    See § 2.3 Overriding the Color Scheme.
    // FIXME: We don't currently support setting an "overriding preference" for color schemes.

    // 3. Otherwise, if the user agent supports at least one of the listed color schemes,
    //    the used color scheme is the first supported color scheme in the list.
    auto first_supported = scheme_value.schemes().first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != CSS::PreferredColorScheme::Auto; });
    if (first_supported.has_value())
        return preferred_color_scheme_from_string(first_supported.value());

    // 4. Otherwise, the used color scheme is the browser default. (Same as normal.)
    // `normal` indicates that the element supports the page’s supported color schemes, if they are set
    if (document_supported_schemes.has_value()) {
        if (preferred_scheme != CSS::PreferredColorScheme::Auto && document_supported_schemes->contains_slow(preferred_color_scheme_to_string(preferred_scheme)))
            return preferred_scheme;

        auto document_first_supported = document_supported_schemes->first_matching([](auto scheme) { return preferred_color_scheme_from_string(scheme) != CSS::PreferredColorScheme::Auto; });
        if (document_first_supported.has_value())
            return preferred_color_scheme_from_string(document_first_supported.value());
    }

    return CSS::PreferredColorScheme::Light;
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

CSSPixels ComputedProperties::compute_line_height(CSSPixelRect const& viewport_rect, Length::FontMetrics const& font_metrics, Length::FontMetrics const& root_font_metrics) const
{
    auto const& line_height = property(CSS::PropertyID::LineHeight);

    if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
        return font_metrics.line_height;

    if (line_height.is_length()) {
        auto line_height_length = line_height.as_length().length();
        if (!line_height_length.is_auto())
            return line_height_length.to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    if (line_height.is_number())
        return Length(line_height.as_number().number(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);

    if (line_height.is_percentage()) {
        // Percentages are relative to 1em. https://www.w3.org/TR/css-inline-3/#valdef-line-height-percentage
        auto const& percentage = line_height.as_percentage().percentage();
        return Length(percentage.as_fraction(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    if (line_height.is_calculated()) {
        if (line_height.as_calculated().resolves_to_number()) {
            auto resolved = line_height.as_calculated().resolve_number();
            if (!resolved.has_value()) {
                dbgln("FIXME: Failed to resolve calc() line-height (number): {}", line_height.as_calculated().to_string(CSSStyleValue::SerializationMode::Normal));
                return CSSPixels::nearest_value_for(m_font_list->first().pixel_metrics().line_spacing());
            }
            return Length(resolved.value(), Length::Type::Em).to_px(viewport_rect, font_metrics, root_font_metrics);
        }

        auto resolved = line_height.as_calculated().resolve_length(Length::ResolutionContext { viewport_rect, font_metrics, root_font_metrics });
        if (!resolved.has_value()) {
            dbgln("FIXME: Failed to resolve calc() line-height: {}", line_height.as_calculated().to_string(CSSStyleValue::SerializationMode::Normal));
            return CSSPixels::nearest_value_for(m_font_list->first().pixel_metrics().line_spacing());
        }
        return resolved->to_px(viewport_rect, font_metrics, root_font_metrics);
    }

    return font_metrics.line_height;
}

Optional<int> ComputedProperties::z_index() const
{
    auto const& value = property(CSS::PropertyID::ZIndex);
    if (value.has_auto())
        return {};
    if (value.is_integer()) {
        // Clamp z-index to the range of a signed 32-bit integer for consistency with other engines.
        auto integer = value.as_integer().integer();
        if (integer >= NumericLimits<int>::max())
            return NumericLimits<int>::max();
        if (integer <= NumericLimits<int>::min())
            return NumericLimits<int>::min();
        return static_cast<int>(integer);
    }
    return {};
}

float ComputedProperties::resolve_opacity_value(CSSStyleValue const& value)
{
    float unclamped_opacity = 1.0f;

    if (value.is_number()) {
        unclamped_opacity = value.as_number().number();
    } else if (value.is_calculated()) {
        auto const& calculated = value.as_calculated();
        if (calculated.resolves_to_percentage()) {
            auto maybe_percentage = value.as_calculated().resolve_percentage();
            if (maybe_percentage.has_value())
                unclamped_opacity = maybe_percentage->as_fraction();
            else
                dbgln("Unable to resolve calc() as opacity (percentage): {}", value.to_string(CSSStyleValue::SerializationMode::Normal));
        } else if (calculated.resolves_to_number()) {
            auto maybe_number = value.as_calculated().resolve_number();
            if (maybe_number.has_value())
                unclamped_opacity = maybe_number.value();
            else
                dbgln("Unable to resolve calc() as opacity (number): {}", value.to_string(CSSStyleValue::SerializationMode::Normal));
        }
    } else if (value.is_percentage()) {
        unclamped_opacity = value.as_percentage().percentage().as_fraction();
    }

    return clamp(unclamped_opacity, 0.0f, 1.0f);
}

float ComputedProperties::opacity() const
{
    auto const& value = property(CSS::PropertyID::Opacity);
    return resolve_opacity_value(value);
}

float ComputedProperties::fill_opacity() const
{
    auto const& value = property(CSS::PropertyID::FillOpacity);
    return resolve_opacity_value(value);
}

Optional<CSS::StrokeLinecap> ComputedProperties::stroke_linecap() const
{
    auto const& value = property(CSS::PropertyID::StrokeLinecap);
    return keyword_to_stroke_linecap(value.to_keyword());
}

Optional<CSS::StrokeLinejoin> ComputedProperties::stroke_linejoin() const
{
    auto const& value = property(CSS::PropertyID::StrokeLinejoin);
    return keyword_to_stroke_linejoin(value.to_keyword());
}

NumberOrCalculated ComputedProperties::stroke_miterlimit() const
{
    auto const& value = property(CSS::PropertyID::StrokeMiterlimit);

    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        VERIFY(math_value.resolves_to_number());
        return NumberOrCalculated { math_value };
    }

    return NumberOrCalculated { value.as_number().number() };
}

float ComputedProperties::stroke_opacity() const
{
    auto const& value = property(CSS::PropertyID::StrokeOpacity);
    return resolve_opacity_value(value);
}

float ComputedProperties::stop_opacity() const
{
    auto const& value = property(CSS::PropertyID::StopOpacity);
    return resolve_opacity_value(value);
}

Optional<CSS::FillRule> ComputedProperties::fill_rule() const
{
    auto const& value = property(CSS::PropertyID::FillRule);
    return keyword_to_fill_rule(value.to_keyword());
}

Optional<CSS::ClipRule> ComputedProperties::clip_rule() const
{
    auto const& value = property(CSS::PropertyID::ClipRule);
    return keyword_to_fill_rule(value.to_keyword());
}

Optional<CSS::FlexDirection> ComputedProperties::flex_direction() const
{
    auto const& value = property(CSS::PropertyID::FlexDirection);
    return keyword_to_flex_direction(value.to_keyword());
}

Optional<CSS::FlexWrap> ComputedProperties::flex_wrap() const
{
    auto const& value = property(CSS::PropertyID::FlexWrap);
    return keyword_to_flex_wrap(value.to_keyword());
}

Optional<CSS::FlexBasis> ComputedProperties::flex_basis() const
{
    auto const& value = property(CSS::PropertyID::FlexBasis);

    if (value.is_keyword() && value.to_keyword() == CSS::Keyword::Content)
        return CSS::FlexBasisContent {};

    return size_value(CSS::PropertyID::FlexBasis);
}

float ComputedProperties::flex_grow() const
{
    auto const& value = property(CSS::PropertyID::FlexGrow);
    if (!value.is_number())
        return 0;
    return value.as_number().number();
}

float ComputedProperties::flex_shrink() const
{
    auto const& value = property(CSS::PropertyID::FlexShrink);
    if (!value.is_number())
        return 1;
    return value.as_number().number();
}

int ComputedProperties::order() const
{
    auto const& value = property(CSS::PropertyID::Order);
    if (!value.is_integer())
        return 0;
    return value.as_integer().integer();
}

Optional<CSS::ImageRendering> ComputedProperties::image_rendering() const
{
    auto const& value = property(CSS::PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword());
}

CSS::Length ComputedProperties::border_spacing_horizontal(Layout::Node const& layout_node) const
{
    auto const& value = property(CSS::PropertyID::BorderSpacing);
    if (value.is_length())
        return value.as_length().length();
    if (value.is_calculated())
        return value.as_calculated().resolve_length(layout_node).value_or(CSS::Length(0, CSS::Length::Type::Px));
    auto const& list = value.as_value_list();
    return list.value_at(0, false)->as_length().length();
}

CSS::Length ComputedProperties::border_spacing_vertical(Layout::Node const& layout_node) const
{
    auto const& value = property(CSS::PropertyID::BorderSpacing);
    if (value.is_length())
        return value.as_length().length();
    if (value.is_calculated())
        return value.as_calculated().resolve_length(layout_node).value_or(CSS::Length(0, CSS::Length::Type::Px));
    auto const& list = value.as_value_list();
    return list.value_at(1, false)->as_length().length();
}

Optional<CSS::CaptionSide> ComputedProperties::caption_side() const
{
    auto const& value = property(CSS::PropertyID::CaptionSide);
    return keyword_to_caption_side(value.to_keyword());
}

CSS::Clip ComputedProperties::clip() const
{
    auto const& value = property(CSS::PropertyID::Clip);
    if (!value.is_rect())
        return CSS::Clip::make_auto();
    return CSS::Clip(value.as_rect().rect());
}

Optional<CSS::JustifyContent> ComputedProperties::justify_content() const
{
    auto const& value = property(CSS::PropertyID::JustifyContent);
    return keyword_to_justify_content(value.to_keyword());
}

Optional<CSS::JustifyItems> ComputedProperties::justify_items() const
{
    auto const& value = property(CSS::PropertyID::JustifyItems);
    return keyword_to_justify_items(value.to_keyword());
}

Optional<CSS::JustifySelf> ComputedProperties::justify_self() const
{
    auto const& value = property(CSS::PropertyID::JustifySelf);
    return keyword_to_justify_self(value.to_keyword());
}

Vector<Transformation> ComputedProperties::transformations_for_style_value(CSSStyleValue const& value)
{
    if (value.is_keyword() && value.to_keyword() == CSS::Keyword::None)
        return {};

    if (!value.is_value_list())
        return {};

    auto& list = value.as_value_list();

    Vector<Transformation> transformations;
    for (auto& it : list.values()) {
        if (!it->is_transformation())
            return {};
        transformations.append(it->as_transformation().to_transformation());
    }
    return transformations;
}

Vector<CSS::Transformation> ComputedProperties::transformations() const
{
    return transformations_for_style_value(property(CSS::PropertyID::Transform));
}

Optional<Transformation> ComputedProperties::rotate() const
{
    auto const& value = property(PropertyID::Rotate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation().to_transformation();
}

Optional<Transformation> ComputedProperties::translate() const
{
    auto const& value = property(PropertyID::Translate);
    if (!value.is_transformation())
        return {};
    return value.as_transformation().to_transformation();
}

Optional<Transformation> ComputedProperties::scale() const
{
    auto const& value = property(PropertyID::Scale);
    if (!value.is_transformation())
        return {};
    return value.as_transformation().to_transformation();
}

static Optional<LengthPercentage> length_percentage_for_style_value(CSSStyleValue const& value)
{
    if (value.is_length())
        return value.as_length().length();
    if (value.is_percentage())
        return value.as_percentage().percentage();
    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };
    return {};
}

Optional<CSS::TransformBox> ComputedProperties::transform_box() const
{
    auto const& value = property(CSS::PropertyID::TransformBox);
    return keyword_to_transform_box(value.to_keyword());
}

CSS::TransformOrigin ComputedProperties::transform_origin() const
{
    auto const& value = property(CSS::PropertyID::TransformOrigin);
    if (!value.is_value_list() || value.as_value_list().size() != 2)
        return {};
    auto const& list = value.as_value_list();
    auto x_value = length_percentage_for_style_value(list.values()[0]);
    auto y_value = length_percentage_for_style_value(list.values()[1]);
    if (!x_value.has_value() || !y_value.has_value()) {
        return {};
    }
    return { x_value.value(), y_value.value() };
}

Optional<Color> ComputedProperties::accent_color(Layout::NodeWithStyle const& node) const
{
    auto const& value = property(CSS::PropertyID::AccentColor);
    if (value.has_color())
        return value.to_color(node);
    return {};
}

Optional<CSS::AlignContent> ComputedProperties::align_content() const
{
    auto const& value = property(CSS::PropertyID::AlignContent);
    return keyword_to_align_content(value.to_keyword());
}

Optional<CSS::AlignItems> ComputedProperties::align_items() const
{
    auto const& value = property(CSS::PropertyID::AlignItems);
    return keyword_to_align_items(value.to_keyword());
}

Optional<CSS::AlignSelf> ComputedProperties::align_self() const
{
    auto const& value = property(CSS::PropertyID::AlignSelf);
    return keyword_to_align_self(value.to_keyword());
}

Optional<CSS::Appearance> ComputedProperties::appearance() const
{
    auto const& value = property(CSS::PropertyID::Appearance);
    auto appearance = keyword_to_appearance(value.to_keyword());
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

CSS::Filter ComputedProperties::backdrop_filter() const
{
    auto const& value = property(CSS::PropertyID::BackdropFilter);
    if (value.is_filter_value_list())
        return Filter(value.as_filter_value_list());
    return Filter::make_none();
}

CSS::Filter ComputedProperties::filter() const
{
    auto const& value = property(CSS::PropertyID::Filter);
    if (value.is_filter_value_list())
        return Filter(value.as_filter_value_list());
    return Filter::make_none();
}

Optional<CSS::Positioning> ComputedProperties::position() const
{
    auto const& value = property(CSS::PropertyID::Position);
    return keyword_to_positioning(value.to_keyword());
}

bool ComputedProperties::operator==(ComputedProperties const& other) const
{
    if (m_property_values.size() != other.m_property_values.size())
        return false;

    for (size_t i = 0; i < m_property_values.size(); ++i) {
        auto const& my_style = m_property_values[i];
        auto const& other_style = other.m_property_values[i];
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

Optional<CSS::TextAnchor> ComputedProperties::text_anchor() const
{
    auto const& value = property(CSS::PropertyID::TextAnchor);
    return keyword_to_text_anchor(value.to_keyword());
}

Optional<CSS::TextAlign> ComputedProperties::text_align() const
{
    auto const& value = property(CSS::PropertyID::TextAlign);
    return keyword_to_text_align(value.to_keyword());
}

Optional<CSS::TextJustify> ComputedProperties::text_justify() const
{
    auto const& value = property(CSS::PropertyID::TextJustify);
    return keyword_to_text_justify(value.to_keyword());
}

Optional<CSS::TextOverflow> ComputedProperties::text_overflow() const
{
    auto const& value = property(CSS::PropertyID::TextOverflow);
    return keyword_to_text_overflow(value.to_keyword());
}

Optional<CSS::PointerEvents> ComputedProperties::pointer_events() const
{
    auto const& value = property(CSS::PropertyID::PointerEvents);
    return keyword_to_pointer_events(value.to_keyword());
}

Variant<LengthOrCalculated, NumberOrCalculated> ComputedProperties::tab_size() const
{
    auto const& value = property(CSS::PropertyID::TabSize);
    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
        if (math_value.resolves_to_number()) {
            return NumberOrCalculated { math_value };
        }
    }

    if (value.is_length())
        return LengthOrCalculated { value.as_length().length() };

    return NumberOrCalculated { value.as_number().number() };
}

Optional<CSS::WordBreak> ComputedProperties::word_break() const
{
    auto const& value = property(CSS::PropertyID::WordBreak);
    return keyword_to_word_break(value.to_keyword());
}

Optional<CSS::LengthOrCalculated> ComputedProperties::word_spacing() const
{
    auto const& value = property(CSS::PropertyID::WordSpacing);
    if (value.is_calculated()) {
        auto& math_value = value.as_calculated();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
    }

    if (value.is_length())
        return LengthOrCalculated { value.as_length().length() };

    return {};
}

Optional<CSS::WhiteSpace> ComputedProperties::white_space() const
{
    auto const& value = property(CSS::PropertyID::WhiteSpace);
    return keyword_to_white_space(value.to_keyword());
}

Optional<LengthOrCalculated> ComputedProperties::letter_spacing() const
{
    auto const& value = property(CSS::PropertyID::LetterSpacing);
    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        if (math_value.resolves_to_length()) {
            return LengthOrCalculated { math_value };
        }
    }

    if (value.is_length())
        return LengthOrCalculated { value.as_length().length() };

    return {};
}

Optional<CSS::LineStyle> ComputedProperties::line_style(CSS::PropertyID property_id) const
{
    auto const& value = property(property_id);
    return keyword_to_line_style(value.to_keyword());
}

Optional<CSS::OutlineStyle> ComputedProperties::outline_style() const
{
    auto const& value = property(CSS::PropertyID::OutlineStyle);
    return keyword_to_outline_style(value.to_keyword());
}

Optional<CSS::Float> ComputedProperties::float_() const
{
    auto const& value = property(CSS::PropertyID::Float);
    return keyword_to_float(value.to_keyword());
}

Optional<CSS::Clear> ComputedProperties::clear() const
{
    auto const& value = property(CSS::PropertyID::Clear);
    return keyword_to_clear(value.to_keyword());
}

Optional<CSS::ColumnSpan> ComputedProperties::column_span() const
{
    auto const& value = property(CSS::PropertyID::ColumnSpan);
    return keyword_to_column_span(value.to_keyword());
}

ComputedProperties::ContentDataAndQuoteNestingLevel ComputedProperties::content(DOM::Element& element, u32 initial_quote_nesting_level) const
{
    auto const& value = property(CSS::PropertyID::Content);
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

    if (value.is_content()) {
        auto& content_style_value = value.as_content();

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
                    dbgln("`{}` is not supported in `content` (yet?)", item->to_string(CSSStyleValue::SerializationMode::Normal));
                    break;
                }
            } else if (item->is_counter()) {
                builder.append(item->as_counter().resolve(element));
            } else {
                // TODO: Implement images, and other things.
                dbgln("`{}` is not supported in `content` (yet?)", item->to_string(CSSStyleValue::SerializationMode::Normal));
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
                    dbgln("`{}` is not supported in `content` alt-text (yet?)", item->to_string(CSSStyleValue::SerializationMode::Normal));
                }
            }
            content_data.alt_text = MUST(alt_text_builder.to_string());
        }

        return { content_data, quote_nesting_level };
    }

    switch (value.to_keyword()) {
    case Keyword::None:
        return { { ContentData::Type::None }, quote_nesting_level };
    case Keyword::Normal:
        return { { ContentData::Type::Normal }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
}

Optional<CSS::ContentVisibility> ComputedProperties::content_visibility() const
{
    auto const& value = property(CSS::PropertyID::ContentVisibility);
    return keyword_to_content_visibility(value.to_keyword());
}

Optional<CSS::Cursor> ComputedProperties::cursor() const
{
    auto const& value = property(CSS::PropertyID::Cursor);
    return keyword_to_cursor(value.to_keyword());
}

Optional<CSS::Visibility> ComputedProperties::visibility() const
{
    auto const& value = property(CSS::PropertyID::Visibility);
    if (!value.is_keyword())
        return {};
    return keyword_to_visibility(value.to_keyword());
}

Display ComputedProperties::display() const
{
    auto const& value = property(PropertyID::Display);
    if (value.is_display()) {
        return value.as_display().display();
    }
    return Display::from_short(Display::Short::Inline);
}

Vector<CSS::TextDecorationLine> ComputedProperties::text_decoration_line() const
{
    auto const& value = property(CSS::PropertyID::TextDecorationLine);

    if (value.is_value_list()) {
        Vector<CSS::TextDecorationLine> lines;
        auto& values = value.as_value_list().values();
        for (auto const& item : values) {
            lines.append(keyword_to_text_decoration_line(item->to_keyword()).value());
        }
        return lines;
    }

    if (value.is_keyword() && value.to_keyword() == Keyword::None)
        return {};

    dbgln("FIXME: Unsupported value for text-decoration-line: {}", value.to_string(CSSStyleValue::SerializationMode::Normal));
    return {};
}

Optional<CSS::TextDecorationStyle> ComputedProperties::text_decoration_style() const
{
    auto const& value = property(CSS::PropertyID::TextDecorationStyle);
    return keyword_to_text_decoration_style(value.to_keyword());
}

Optional<CSS::TextTransform> ComputedProperties::text_transform() const
{
    auto const& value = property(CSS::PropertyID::TextTransform);
    return keyword_to_text_transform(value.to_keyword());
}

Optional<CSS::ListStyleType> ComputedProperties::list_style_type() const
{
    auto const& value = property(CSS::PropertyID::ListStyleType);
    return keyword_to_list_style_type(value.to_keyword());
}

Optional<CSS::ListStylePosition> ComputedProperties::list_style_position() const
{
    auto const& value = property(CSS::PropertyID::ListStylePosition);
    return keyword_to_list_style_position(value.to_keyword());
}

Optional<CSS::Overflow> ComputedProperties::overflow_x() const
{
    return overflow(CSS::PropertyID::OverflowX);
}

Optional<CSS::Overflow> ComputedProperties::overflow_y() const
{
    return overflow(CSS::PropertyID::OverflowY);
}

Optional<CSS::Overflow> ComputedProperties::overflow(CSS::PropertyID property_id) const
{
    auto const& value = property(property_id);
    return keyword_to_overflow(value.to_keyword());
}

Vector<ShadowData> ComputedProperties::shadow(PropertyID property_id, Layout::Node const& layout_node) const
{
    auto const& value = property(property_id);

    auto resolve_to_length = [&layout_node](NonnullRefPtr<CSSStyleValue const> const& value) -> Optional<Length> {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_calculated())
            return value->as_calculated().resolve_length(layout_node);
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
            maybe_offset_x.release_value(),
            maybe_offset_y.release_value(),
            maybe_blur_radius.release_value(),
            maybe_spread_distance.release_value(),
            value.color()->to_color(as<Layout::NodeWithStyle>(layout_node)),
            value.placement()
        };
    };

    if (value.is_value_list()) {
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

    if (value.is_shadow()) {
        auto maybe_shadow_data = make_shadow_data(value.as_shadow());
        if (!maybe_shadow_data.has_value())
            return {};
        return { maybe_shadow_data.release_value() };
    }

    return {};
}

Vector<ShadowData> ComputedProperties::box_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::BoxShadow, layout_node);
}

Vector<ShadowData> ComputedProperties::text_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::TextShadow, layout_node);
}

Optional<CSS::BoxSizing> ComputedProperties::box_sizing() const
{
    auto const& value = property(CSS::PropertyID::BoxSizing);
    return keyword_to_box_sizing(value.to_keyword());
}

Variant<CSS::VerticalAlign, CSS::LengthPercentage> ComputedProperties::vertical_align() const
{
    auto const& value = property(CSS::PropertyID::VerticalAlign);

    if (value.is_keyword())
        return keyword_to_vertical_align(value.to_keyword()).release_value();

    if (value.is_length())
        return CSS::LengthPercentage(value.as_length().length());

    if (value.is_percentage())
        return CSS::LengthPercentage(value.as_percentage().percentage());

    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };

    VERIFY_NOT_REACHED();
}

Optional<FlyString> ComputedProperties::font_language_override() const
{
    auto const& value = property(CSS::PropertyID::FontLanguageOverride);
    if (value.is_string())
        return value.as_string().string_value();
    return {};
}

Optional<Gfx::FontVariantAlternates> ComputedProperties::font_variant_alternates() const
{
    auto const& value = property(CSS::PropertyID::FontVariantAlternates);
    return value.to_font_variant_alternates();
}

Optional<FontVariantCaps> ComputedProperties::font_variant_caps() const
{
    auto const& value = property(CSS::PropertyID::FontVariantCaps);
    return value.to_font_variant_caps();
}

Optional<Gfx::FontVariantEastAsian> ComputedProperties::font_variant_east_asian() const
{
    auto const& value = property(CSS::PropertyID::FontVariantEastAsian);
    return value.to_font_variant_east_asian();
}

Optional<FontVariantEmoji> ComputedProperties::font_variant_emoji() const
{
    auto const& value = property(CSS::PropertyID::FontVariantEmoji);
    return value.to_font_variant_emoji();
}

Optional<Gfx::FontVariantLigatures> ComputedProperties::font_variant_ligatures() const
{
    auto const& value = property(CSS::PropertyID::FontVariantLigatures);
    return value.to_font_variant_ligatures();
}

Optional<Gfx::FontVariantNumeric> ComputedProperties::font_variant_numeric() const
{
    auto const& value = property(CSS::PropertyID::FontVariantNumeric);
    return value.to_font_variant_numeric();
}

Optional<FontVariantPosition> ComputedProperties::font_variant_position() const
{
    auto const& value = property(CSS::PropertyID::FontVariantPosition);
    return value.to_font_variant_position();
}

Optional<HashMap<FlyString, IntegerOrCalculated>> ComputedProperties::font_feature_settings() const
{
    auto const& value = property(PropertyID::FontFeatureSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& feature_tags = value.as_value_list().values();
        HashMap<FlyString, IntegerOrCalculated> result;
        result.ensure_capacity(feature_tags.size());
        for (auto const& tag_value : feature_tags) {
            auto const& feature_tag = tag_value->as_open_type_tagged();

            if (feature_tag.value()->is_integer()) {
                result.set(feature_tag.tag(), feature_tag.value()->as_integer().value());
            } else {
                VERIFY(feature_tag.value()->is_calculated());
                result.set(feature_tag.tag(), IntegerOrCalculated { feature_tag.value()->as_calculated() });
            }
        }
        return result;
    }

    return {};
}

Optional<HashMap<FlyString, NumberOrCalculated>> ComputedProperties::font_variation_settings() const
{
    auto const& value = property(CSS::PropertyID::FontVariationSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& axis_tags = value.as_value_list().values();
        HashMap<FlyString, NumberOrCalculated> result;
        result.ensure_capacity(axis_tags.size());
        for (auto const& tag_value : axis_tags) {
            auto const& axis_tag = tag_value->as_open_type_tagged();

            if (axis_tag.value()->is_number()) {
                result.set(axis_tag.tag(), axis_tag.value()->as_number().value());
            } else {
                VERIFY(axis_tag.value()->is_calculated());
                result.set(axis_tag.tag(), NumberOrCalculated { axis_tag.value()->as_calculated() });
            }
        }
        return result;
    }

    return {};
}

CSS::GridTrackSizeList ComputedProperties::grid_auto_columns() const
{
    auto const& value = property(CSS::PropertyID::GridAutoColumns);
    return value.as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList ComputedProperties::grid_auto_rows() const
{
    auto const& value = property(CSS::PropertyID::GridAutoRows);
    return value.as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList ComputedProperties::grid_template_columns() const
{
    auto const& value = property(CSS::PropertyID::GridTemplateColumns);
    return value.as_grid_track_size_list().grid_track_size_list();
}

CSS::GridTrackSizeList ComputedProperties::grid_template_rows() const
{
    auto const& value = property(CSS::PropertyID::GridTemplateRows);
    return value.as_grid_track_size_list().grid_track_size_list();
}

CSS::GridAutoFlow ComputedProperties::grid_auto_flow() const
{
    auto const& value = property(CSS::PropertyID::GridAutoFlow);
    if (!value.is_grid_auto_flow())
        return CSS::GridAutoFlow {};
    auto& grid_auto_flow_value = value.as_grid_auto_flow();
    return CSS::GridAutoFlow { .row = grid_auto_flow_value.is_row(), .dense = grid_auto_flow_value.is_dense() };
}

CSS::GridTrackPlacement ComputedProperties::grid_column_end() const
{
    auto const& value = property(CSS::PropertyID::GridColumnEnd);
    return value.as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement ComputedProperties::grid_column_start() const
{
    auto const& value = property(CSS::PropertyID::GridColumnStart);
    return value.as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement ComputedProperties::grid_row_end() const
{
    auto const& value = property(CSS::PropertyID::GridRowEnd);
    return value.as_grid_track_placement().grid_track_placement();
}

CSS::GridTrackPlacement ComputedProperties::grid_row_start() const
{
    auto const& value = property(CSS::PropertyID::GridRowStart);
    return value.as_grid_track_placement().grid_track_placement();
}

Optional<CSS::BorderCollapse> ComputedProperties::border_collapse() const
{
    auto const& value = property(CSS::PropertyID::BorderCollapse);
    return keyword_to_border_collapse(value.to_keyword());
}

Vector<Vector<String>> ComputedProperties::grid_template_areas() const
{
    auto const& value = property(CSS::PropertyID::GridTemplateAreas);
    return value.as_grid_template_area().grid_template_area();
}

Optional<CSS::ObjectFit> ComputedProperties::object_fit() const
{
    auto const& value = property(CSS::PropertyID::ObjectFit);
    return keyword_to_object_fit(value.to_keyword());
}

CSS::ObjectPosition ComputedProperties::object_position() const
{
    auto const& value = property(CSS::PropertyID::ObjectPosition);
    auto const& position = value.as_position();
    CSS::ObjectPosition object_position;
    auto const& edge_x = position.edge_x();
    auto const& edge_y = position.edge_y();
    if (edge_x->is_edge()) {
        auto const& edge = edge_x->as_edge();
        object_position.edge_x = edge.edge().value_or(PositionEdge::Left);
        object_position.offset_x = edge.offset();
    }
    if (edge_y->is_edge()) {
        auto const& edge = edge_y->as_edge();
        object_position.edge_y = edge.edge().value_or(PositionEdge::Top);
        object_position.offset_y = edge.offset();
    }
    return object_position;
}

Optional<CSS::TableLayout> ComputedProperties::table_layout() const
{
    auto const& value = property(CSS::PropertyID::TableLayout);
    return keyword_to_table_layout(value.to_keyword());
}

Optional<CSS::Direction> ComputedProperties::direction() const
{
    auto const& value = property(CSS::PropertyID::Direction);
    return keyword_to_direction(value.to_keyword());
}

Optional<CSS::UnicodeBidi> ComputedProperties::unicode_bidi() const
{
    auto const& value = property(CSS::PropertyID::UnicodeBidi);
    return keyword_to_unicode_bidi(value.to_keyword());
}

Optional<CSS::WritingMode> ComputedProperties::writing_mode() const
{
    auto const& value = property(CSS::PropertyID::WritingMode);
    return keyword_to_writing_mode(value.to_keyword());
}

Optional<CSS::UserSelect> ComputedProperties::user_select() const
{
    auto const& value = property(CSS::PropertyID::UserSelect);
    return keyword_to_user_select(value.to_keyword());
}

Optional<CSS::Isolation> ComputedProperties::isolation() const
{
    auto const& value = property(CSS::PropertyID::Isolation);
    return keyword_to_isolation(value.to_keyword());
}

Optional<CSS::MaskType> ComputedProperties::mask_type() const
{
    auto const& value = property(CSS::PropertyID::MaskType);
    return keyword_to_mask_type(value.to_keyword());
}

Color ComputedProperties::stop_color() const
{
    NonnullRawPtr<CSSStyleValue const> value = property(CSS::PropertyID::StopColor);
    if (value->is_keyword()) {
        // Workaround lack of layout node to resolve current color.
        auto const& keyword = value->as_keyword();
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

void ComputedProperties::set_math_depth(int math_depth)
{
    m_math_depth = math_depth;
    // Make our children inherit our computed value, not our specified value.
    set_property(PropertyID::MathDepth, MathDepthStyleValue::create_integer(IntegerStyleValue::create(math_depth)));
}

QuotesData ComputedProperties::quotes() const
{
    auto const& value = property(CSS::PropertyID::Quotes);
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
            if (counter.value) {
                if (counter.value->is_integer()) {
                    data.value = AK::clamp_to<i32>(counter.value->as_integer().integer());
                } else if (counter.value->is_calculated()) {
                    auto maybe_int = counter.value->as_calculated().resolve_integer();
                    if (maybe_int.has_value())
                        data.value = AK::clamp_to<i32>(*maybe_int);
                } else {
                    dbgln("Unimplemented type for {} integer value: '{}'", string_from_property_id(property_id), counter.value->to_string(CSSStyleValue::SerializationMode::Normal));
                }
            }
            result.append(move(data));
        }
        return result;
    }

    if (value.to_keyword() == Keyword::None)
        return {};

    dbgln("Unhandled type for {} value: '{}'", string_from_property_id(property_id), value.to_string(CSSStyleValue::SerializationMode::Normal));
    return {};
}

Optional<CSS::ScrollbarWidth> ComputedProperties::scrollbar_width() const
{
    auto const& value = property(CSS::PropertyID::ScrollbarWidth);
    return keyword_to_scrollbar_width(value.to_keyword());
}

}
