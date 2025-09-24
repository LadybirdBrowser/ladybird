/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
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
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>
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

bool ComputedProperties::is_property_important(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    return m_property_important[n / 8] & (1 << (n % 8));
}

void ComputedProperties::set_property_important(PropertyID property_id, Important important)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    if (important == Important::Yes)
        m_property_important[n / 8] |= (1 << (n % 8));
    else
        m_property_important[n / 8] &= ~(1 << (n % 8));
}

bool ComputedProperties::is_property_inherited(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    return m_property_inherited[n / 8] & (1 << (n % 8));
}

bool ComputedProperties::is_animated_property_inherited(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    return m_animated_property_inherited[n / 8] & (1 << (n % 8));
}

void ComputedProperties::set_property_inherited(PropertyID property_id, Inherited inherited)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    if (inherited == Inherited::Yes)
        m_property_inherited[n / 8] |= (1 << (n % 8));
    else
        m_property_inherited[n / 8] &= ~(1 << (n % 8));
}

void ComputedProperties::set_animated_property_inherited(PropertyID property_id, Inherited inherited)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    if (inherited == Inherited::Yes)
        m_animated_property_inherited[n / 8] |= (1 << (n % 8));
    else
        m_animated_property_inherited[n / 8] &= ~(1 << (n % 8));
}

void ComputedProperties::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited, Important important)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = move(value);
    set_property_important(id, important);
    set_property_inherited(id, inherited);
}

void ComputedProperties::revert_property(PropertyID id, ComputedProperties const& style_for_revert)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = style_for_revert.m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)];
    set_property_important(id, style_for_revert.is_property_important(id) ? Important::Yes : Important::No);
    set_property_inherited(id, style_for_revert.is_property_inherited(id) ? Inherited::Yes : Inherited::No);
}

void ComputedProperties::set_animated_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited)
{
    m_animated_property_values.set(id, move(value));
    set_animated_property_inherited(id, inherited);
}

void ComputedProperties::remove_animated_property(PropertyID id)
{
    m_animated_property_values.remove(id);
}

void ComputedProperties::reset_animated_properties(Badge<Animations::KeyframeEffect>)
{
    m_animated_property_values.clear();
}

StyleValue const& ComputedProperties::property(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    if (return_animated_value == WithAnimationsApplied::Yes) {
        if (auto animated_value = m_animated_property_values.get(property_id); animated_value.has_value())
            return *animated_value.value();
    }

    // By the time we call this method, all properties have values assigned.
    return *m_property_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
}

Variant<LengthPercentage, NormalGap> ComputedProperties::gap_value(PropertyID id) const
{
    auto const& value = property(id);
    if (value.is_keyword()) {
        VERIFY(value.as_keyword().keyword() == Keyword::Normal);
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

Size ComputedProperties::size_value(PropertyID id) const
{
    auto const& value = property(id);
    if (value.is_keyword()) {
        switch (value.to_keyword()) {
        case Keyword::Auto:
            return Size::make_auto();
        case Keyword::MinContent:
            return Size::make_min_content();
        case Keyword::MaxContent:
            return Size::make_max_content();
        case Keyword::None:
            return Size::make_none();
        default:
            VERIFY_NOT_REACHED();
        }
    }
    if (value.is_fit_content()) {
        auto& fit_content = value.as_fit_content();
        if (auto length_percentage = fit_content.length_percentage(); length_percentage.has_value())
            return Size::make_fit_content(length_percentage.release_value());
        return Size::make_fit_content();
    }

    if (value.is_calculated())
        return Size::make_calculated(value.as_calculated());

    if (value.is_percentage())
        return Size::make_percentage(value.as_percentage().percentage());

    if (value.is_length())
        return Size::make_length(value.as_length().length());

    // FIXME: Support `anchor-size(..)`
    if (value.is_anchor_size())
        return Size::make_none();

    dbgln("FIXME: Unsupported size value: `{}`, treating as `auto`", value.to_string(SerializationMode::Normal));
    return Size::make_auto();
}

Optional<LengthPercentage> ComputedProperties::length_percentage(PropertyID id, Layout::NodeWithStyle const& layout_node, ClampNegativeLengths disallow_negative_lengths) const
{
    auto const& value = property(id);

    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };

    if (value.is_percentage()) {
        auto percentage = value.as_percentage().percentage();

        // FIXME: This value can be negative as interpolation does not yet clamp values to allowed ranges - remove this
        //        once we do that.
        if (disallow_negative_lengths == ClampNegativeLengths::Yes && percentage.as_fraction() < 0)
            return {};

        return percentage;
    }

    if (value.is_length()) {
        auto length = value.as_length().length();

        // FIXME: This value can be negative as interpolation does not yet clamp values to allowed ranges - remove this
        //        once we do that.
        if (disallow_negative_lengths == ClampNegativeLengths::Yes && length.to_px(layout_node) < 0)
            return {};

        return length;
    }

    return {};
}

Length ComputedProperties::length(PropertyID property_id) const
{
    return property(property_id).as_length().length();
}

LengthBox ComputedProperties::length_box(PropertyID left_id, PropertyID top_id, PropertyID right_id, PropertyID bottom_id, Layout::NodeWithStyle const& layout_node, ClampNegativeLengths disallow_negative_lengths, LengthPercentageOrAuto const& default_value) const
{
    auto length_box_side = [&](PropertyID id) -> LengthPercentageOrAuto {
        auto const& value = property(id);

        if (value.is_calculated())
            return LengthPercentage { value.as_calculated() };

        if (value.is_percentage()) {
            auto percentage = value.as_percentage().percentage();

            // FIXME: This value can be negative as interpolation does not yet clamp values to allowed ranges - remove this
            //        once we do that.
            if (disallow_negative_lengths == ClampNegativeLengths::Yes && percentage.as_fraction() < 0)
                return default_value;

            return percentage;
        }

        if (value.is_length()) {
            auto length = value.as_length().length();

            // FIXME: This value can be negative as interpolation does not yet clamp values to allowed ranges - remove this
            //        once we do that.
            if (disallow_negative_lengths == ClampNegativeLengths::Yes && length.to_px(layout_node) < 0)
                return default_value;

            return value.as_length().length();
        }

        if (value.has_auto())
            return LengthPercentageOrAuto::make_auto();

        return default_value;
    };

    return LengthBox {
        length_box_side(top_id),
        length_box_side(right_id),
        length_box_side(bottom_id),
        length_box_side(left_id)
    };
}

Color ComputedProperties::color_or_fallback(PropertyID id, ColorResolutionContext color_resolution_context, Color fallback) const
{
    auto const& value = property(id);
    if (!value.has_color())
        return fallback;
    return value.to_color(color_resolution_context).value();
}

ColorInterpolation ComputedProperties::color_interpolation() const
{
    auto const& value = property(PropertyID::ColorInterpolation);
    return keyword_to_color_interpolation(value.to_keyword()).value_or(CSS::ColorInterpolation::Auto);
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
PreferredColorScheme ComputedProperties::color_scheme(PreferredColorScheme preferred_scheme, Optional<Vector<String> const&> document_supported_schemes) const
{
    // To determine the used color scheme of an element:
    auto const& scheme_value = property(PropertyID::ColorScheme).as_color_scheme();

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if (preferred_scheme != PreferredColorScheme::Auto && scheme_value.schemes().contains_slow(preferred_color_scheme_to_string(preferred_scheme)))
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
        if (preferred_scheme != PreferredColorScheme::Auto && document_supported_schemes->contains_slow(preferred_color_scheme_to_string(preferred_scheme)))
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

CSSPixels ComputedProperties::line_height() const
{
    // https://drafts.csswg.org/css-inline-3/#line-height-property
    auto const& line_height = property(PropertyID::LineHeight);

    // normal
    // Determine the preferred line height automatically based on font metrics.
    if (line_height.is_keyword() && line_height.to_keyword() == Keyword::Normal)
        return CSSPixels { round_to<i32>(font_size() * normal_line_height_scale) };

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

Optional<int> ComputedProperties::z_index() const
{
    auto const& value = property(PropertyID::ZIndex);
    if (value.has_auto())
        return {};

    // Clamp z-index to the range of a signed 32-bit integer for consistency with other engines.
    if (value.is_integer()) {
        auto number = value.as_integer().integer();

        if (number >= NumericLimits<int>::max())
            return NumericLimits<int>::max();
        if (number <= NumericLimits<int>::min())
            return NumericLimits<int>::min();

        return value.as_integer().integer();
    }

    if (value.is_calculated()) {
        auto maybe_double = value.as_calculated().resolve_number({});
        if (maybe_double.has_value()) {
            if (*maybe_double >= NumericLimits<int>::max())
                return NumericLimits<int>::max();

            if (*maybe_double <= NumericLimits<int>::min())
                return NumericLimits<int>::min();

            // Round up on half
            return floor(maybe_double.value() + 0.5f);
        }
    }
    return {};
}

float ComputedProperties::opacity() const
{
    return property(PropertyID::Opacity).as_number().number();
}

float ComputedProperties::fill_opacity() const
{
    return property(PropertyID::FillOpacity).as_number().number();
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

NumberOrCalculated ComputedProperties::stroke_miterlimit() const
{
    auto const& value = property(PropertyID::StrokeMiterlimit);

    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        VERIFY(math_value.resolves_to_number());
        return NumberOrCalculated { math_value };
    }

    return NumberOrCalculated { value.as_number().number() };
}

float ComputedProperties::stroke_opacity() const
{
    return property(PropertyID::StrokeOpacity).as_number().number();
}

float ComputedProperties::stop_opacity() const
{
    return property(PropertyID::StopOpacity).as_number().number();
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
    return property(PropertyID::FloodOpacity).as_number().number();
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

float ComputedProperties::flex_grow() const
{
    auto const& value = property(PropertyID::FlexGrow);
    if (!value.is_number())
        return 0;
    return value.as_number().number();
}

float ComputedProperties::flex_shrink() const
{
    auto const& value = property(PropertyID::FlexShrink);
    if (!value.is_number())
        return 1;
    return value.as_number().number();
}

int ComputedProperties::order() const
{
    auto const& value = property(PropertyID::Order);
    if (!value.is_integer())
        return 0;
    return value.as_integer().integer();
}

ImageRendering ComputedProperties::image_rendering() const
{
    auto const& value = property(PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword()).release_value();
}

Length ComputedProperties::border_spacing_horizontal(Layout::Node const& layout_node) const
{
    auto resolve_value = [&](auto const& style_value) -> Optional<Length> {
        if (style_value.is_length())
            return style_value.as_length().length();
        if (style_value.is_calculated())
            return style_value.as_calculated().resolve_length({ .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) }).value_or(Length::make_px(0));
        return {};
    };

    auto const& style_value = property(PropertyID::BorderSpacing);
    auto resolved_value = resolve_value(style_value);
    if (!resolved_value.has_value()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 0);
        resolved_value = resolve_value(*list.value_at(0, false));
    }

    VERIFY(resolved_value.has_value());
    return *resolved_value;
}

Length ComputedProperties::border_spacing_vertical(Layout::Node const& layout_node) const
{
    auto resolve_value = [&](auto const& style_value) -> Optional<Length> {
        if (style_value.is_length())
            return style_value.as_length().length();
        if (style_value.is_calculated())
            return style_value.as_calculated().resolve_length({ .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) }).value_or(Length::make_px(0));
        return {};
    };

    auto const& style_value = property(PropertyID::BorderSpacing);
    auto resolved_value = resolve_value(style_value);
    if (!resolved_value.has_value()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 1);
        resolved_value = resolve_value(*list.value_at(1, false));
    }

    VERIFY(resolved_value.has_value());
    return *resolved_value;
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

Vector<Transformation> ComputedProperties::transformations_for_style_value(StyleValue const& value)
{
    if (value.is_keyword() && value.to_keyword() == Keyword::None)
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

Vector<Transformation> ComputedProperties::transformations() const
{
    return transformations_for_style_value(property(PropertyID::Transform));
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

static Optional<LengthPercentage> length_percentage_for_style_value(StyleValue const& value)
{
    if (value.is_length())
        return value.as_length().length();
    if (value.is_percentage())
        return value.as_percentage().percentage();
    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };
    return {};
}

TransformBox ComputedProperties::transform_box() const
{
    auto const& value = property(PropertyID::TransformBox);
    return keyword_to_transform_box(value.to_keyword()).release_value();
}

TransformOrigin ComputedProperties::transform_origin() const
{
    auto length_percentage_with_keywords_resolved = [](StyleValue const& value) -> Optional<LengthPercentage> {
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
        return length_percentage_for_style_value(value);
    };

    auto const& value = property(PropertyID::TransformOrigin);
    if (!value.is_value_list() || value.as_value_list().size() != 3)
        return {};
    auto const& list = value.as_value_list();

    auto x_value = length_percentage_with_keywords_resolved(list.values()[0]);
    auto y_value = length_percentage_with_keywords_resolved(list.values()[1]);
    auto z_value = length_percentage_for_style_value(list.values()[2]);
    if (!x_value.has_value() || !y_value.has_value() || !z_value.has_value())
        return {};
    return { x_value.value(), y_value.value(), z_value.value() };
}

Optional<Color> ComputedProperties::accent_color(Layout::NodeWithStyle const& node) const
{
    auto const& value = property(PropertyID::AccentColor);
    if (value.has_color())
        return value.to_color(ColorResolutionContext::for_layout_node_with_style(node));
    return {};
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
    case Appearance::Textfield:
    case Appearance::MenulistButton:
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
    default:
        break;
    }
    return appearance;
}

Filter ComputedProperties::backdrop_filter() const
{
    auto const& value = property(PropertyID::BackdropFilter);
    if (value.is_filter_value_list())
        return Filter(value.as_filter_value_list());
    return Filter::make_none();
}

Filter ComputedProperties::filter() const
{
    auto const& value = property(PropertyID::Filter);
    if (value.is_filter_value_list())
        return Filter(value.as_filter_value_list());
    return Filter::make_none();
}

Positioning ComputedProperties::position() const
{
    auto const& value = property(PropertyID::Position);
    return keyword_to_positioning(value.to_keyword()).release_value();
}

bool ComputedProperties::operator==(ComputedProperties const& other) const
{
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

TextAnchor ComputedProperties::text_anchor() const
{
    auto const& value = property(PropertyID::TextAnchor);
    return keyword_to_text_anchor(value.to_keyword()).release_value();
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
    if (computed_text_underline_offset.is_length())
        return computed_text_underline_offset.as_length().length().absolute_length_to_px();

    // <percentage>
    if (computed_text_underline_offset.is_percentage())
        return font_size().scaled(computed_text_underline_offset.as_percentage().percentage().as_fraction());

    // NOTE: We also support calc()'d <length-percentage>
    if (computed_text_underline_offset.is_calculated())
        // NOTE: We don't need to pass a length resolution context here as lengths have already been absolutized in
        //       StyleComputer::compute_text_underline_offset
        return computed_text_underline_offset.as_calculated().resolve_length({ .percentage_basis = Length::make_px(font_size()), .length_resolution_context = {} })->absolute_length_to_px();

    VERIFY_NOT_REACHED();
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

Variant<LengthOrCalculated, NumberOrCalculated> ComputedProperties::tab_size() const
{
    auto const& value = property(PropertyID::TabSize);
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

    if (value.is_length())
        return value.as_length().length().absolute_length_to_px();

    if (value.is_percentage())
        return font_size().scale_by(value.as_percentage().percentage().as_fraction());

    if (value.is_calculated())
        return value.as_calculated().resolve_length({ .percentage_basis = Length::make_px(font_size()), .length_resolution_context = {} })->absolute_length_to_px();

    VERIFY_NOT_REACHED();
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

    if (value.is_length())
        return value.as_length().length().absolute_length_to_px();

    if (value.is_percentage())
        return font_size().scale_by(value.as_percentage().percentage().as_fraction());

    if (value.is_calculated())
        return value.as_calculated().resolve_length({ .percentage_basis = Length::make_px(font_size()), .length_resolution_context = {} })->absolute_length_to_px();

    VERIFY_NOT_REACHED();
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

Color ComputedProperties::caret_color(Layout::NodeWithStyle const& node) const
{
    auto const& value = property(PropertyID::CaretColor);
    if (value.is_keyword() && value.to_keyword() == Keyword::Auto)
        return node.computed_values().color();

    if (value.has_color())
        return value.to_color(ColorResolutionContext::for_layout_node_with_style(node)).value();

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

ComputedProperties::ContentDataAndQuoteNestingLevel ComputedProperties::content(DOM::AbstractElement& element_reference, u32 initial_quote_nesting_level) const
{
    auto const& value = property(PropertyID::Content);
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

        ContentData content_data;

        for (auto const& item : content_style_value.content().values()) {
            if (item->is_string()) {
                content_data.data.append(item->as_string().string_value().to_string());
            } else if (item->is_keyword()) {
                switch (item->to_keyword()) {
                case Keyword::OpenQuote:
                    content_data.data.append(get_quote_string(true, quote_nesting_level++).to_string());
                    break;
                case Keyword::CloseQuote:
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    if (quote_nesting_level > 0)
                        content_data.data.append(get_quote_string(false, --quote_nesting_level).to_string());
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
                content_data.data.append(item->as_counter().resolve(element_reference));
            } else if (item->is_image()) {
                content_data.data.append(NonnullRefPtr { const_cast<ImageStyleValue&>(item->as_image()) });
            } else {
                // TODO: Implement images, and other things.
                dbgln("`{}` is not supported in `content` (yet?)", item->to_string(SerializationMode::Normal));
            }
        }
        content_data.type = ContentData::Type::List;

        if (content_style_value.has_alt_text()) {
            StringBuilder alt_text_builder;
            for (auto const& item : content_style_value.alt_text()->values()) {
                if (item->is_string()) {
                    alt_text_builder.append(item->as_string().string_value());
                } else if (item->is_counter()) {
                    alt_text_builder.append(item->as_counter().resolve(element_reference));
                } else {
                    dbgln("`{}` is not supported in `content` alt-text (yet?)", item->to_string(SerializationMode::Normal));
                }
            }
            content_data.alt_text = MUST(alt_text_builder.to_string());
        }

        return { content_data, quote_nesting_level };
    }

    switch (value.to_keyword()) {
    case Keyword::None:
        return { { ContentData::Type::None, {} }, quote_nesting_level };
    case Keyword::Normal:
        return { { ContentData::Type::Normal, {} }, quote_nesting_level };
    default:
        break;
    }

    return { {}, quote_nesting_level };
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
    auto const& value = property(PropertyID::Display);
    if (value.is_display()) {
        return value.as_display().display();
    }
    return Display::from_short(Display::Short::Inline);
}

Vector<TextDecorationLine> ComputedProperties::text_decoration_line() const
{
    auto const& value = property(PropertyID::TextDecorationLine);

    if (value.is_value_list()) {
        Vector<TextDecorationLine> lines;
        auto& values = value.as_value_list().values();
        for (auto const& item : values) {
            lines.append(keyword_to_text_decoration_line(item->to_keyword()).value());
        }
        return lines;
    }

    if (value.is_keyword()) {
        if (value.to_keyword() == Keyword::None)
            return {};
        return { keyword_to_text_decoration_line(value.to_keyword()).release_value() };
    }

    dbgln("FIXME: Unsupported value for text-decoration-line: {}", value.to_string(SerializationMode::Normal));
    return {};
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
    if (value.is_length())
        return TextDecorationThickness { LengthPercentage { value.as_length().length() } };
    if (value.is_percentage())
        return TextDecorationThickness { LengthPercentage { value.as_percentage().percentage() } };
    if (value.is_calculated())
        return TextDecorationThickness { LengthPercentage { value.as_calculated() } };
    VERIFY_NOT_REACHED();
}

TextTransform ComputedProperties::text_transform() const
{
    auto const& value = property(PropertyID::TextTransform);
    return keyword_to_text_transform(value.to_keyword()).release_value();
}

ListStyleType ComputedProperties::list_style_type() const
{
    auto const& value = property(PropertyID::ListStyleType);
    if (value.is_string())
        return value.as_string().string_value().to_string();
    return keyword_to_counter_style_name_keyword(value.to_keyword()).release_value();
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

Vector<ShadowData> ComputedProperties::shadow(PropertyID property_id, Layout::Node const& layout_node) const
{
    auto const& value = property(property_id);

    auto resolve_to_length = [&layout_node](NonnullRefPtr<StyleValue const> const& value) -> Optional<Length> {
        if (value->is_length())
            return value->as_length().length();
        if (value->is_calculated())
            return value->as_calculated().resolve_length({ .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) });
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
            value.color()->to_color(ColorResolutionContext::for_layout_node_with_style(as<Layout::NodeWithStyle>(layout_node))).value(),
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

    if (value.is_length())
        return LengthPercentage(value.as_length().length());

    if (value.is_percentage())
        return LengthPercentage(value.as_percentage().percentage());

    if (value.is_calculated())
        return LengthPercentage { value.as_calculated() };

    VERIFY_NOT_REACHED();
}

FontKerning ComputedProperties::font_kerning() const
{
    auto const& value = property(PropertyID::FontKerning);
    return keyword_to_font_kerning(value.to_keyword()).release_value();
}

Optional<FlyString> ComputedProperties::font_language_override() const
{
    auto const& value = property(PropertyID::FontLanguageOverride);
    if (value.is_string())
        return value.as_string().string_value();
    return {};
}

Optional<Gfx::FontVariantAlternates> ComputedProperties::font_variant_alternates() const
{
    auto const& value = property(PropertyID::FontVariantAlternates);
    switch (keyword_to_font_variant_alternates(value.to_keyword()).value()) {
    case FontVariantAlternates::Normal:
        return {};
    case FontVariantAlternates::HistoricalForms:
        return Gfx::FontVariantAlternates { .historical_forms = true };
    }
    VERIFY_NOT_REACHED();
}

FontVariantCaps ComputedProperties::font_variant_caps() const
{
    auto const& value = property(PropertyID::FontVariantCaps);
    return keyword_to_font_variant_caps(value.to_keyword()).release_value();
}

Optional<Gfx::FontVariantEastAsian> ComputedProperties::font_variant_east_asian() const
{
    auto const& value = property(PropertyID::FontVariantEastAsian);
    Gfx::FontVariantEastAsian east_asian {};
    bool normal = false;

    auto apply_keyword = [&east_asian, &normal](Keyword keyword) {
        switch (keyword) {
        case Keyword::Normal:
            normal = true;
            break;
        case Keyword::Jis78:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Jis78;
            break;
        case Keyword::Jis83:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Jis83;
            break;
        case Keyword::Jis90:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Jis90;
            break;
        case Keyword::Jis04:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Jis04;
            break;
        case Keyword::Simplified:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Simplified;
            break;
        case Keyword::Traditional:
            east_asian.variant = Gfx::FontVariantEastAsian::Variant::Traditional;
            break;
        case Keyword::FullWidth:
            east_asian.width = Gfx::FontVariantEastAsian::Width::FullWidth;
            break;
        case Keyword::ProportionalWidth:
            east_asian.width = Gfx::FontVariantEastAsian::Width::Proportional;
            break;
        case Keyword::Ruby:
            east_asian.ruby = true;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    if (value.is_keyword()) {
        apply_keyword(value.to_keyword());
    } else if (value.is_value_list()) {
        for (auto& child_value : value.as_value_list().values()) {
            apply_keyword(child_value->to_keyword());
        }
    }

    if (normal)
        return {};

    return east_asian;
}

FontVariantEmoji ComputedProperties::font_variant_emoji() const
{
    auto const& value = property(PropertyID::FontVariantEmoji);
    return keyword_to_font_variant_emoji(value.to_keyword()).release_value();
}

Optional<Gfx::FontVariantLigatures> ComputedProperties::font_variant_ligatures() const
{
    auto const& value = property(PropertyID::FontVariantLigatures);
    Gfx::FontVariantLigatures ligatures {};
    bool normal = false;

    auto apply_keyword = [&ligatures, &normal](Keyword keyword) {
        switch (keyword) {
        case Keyword::Normal:
            normal = true;
            break;
        case Keyword::None:
            ligatures.none = true;
            break;
        case Keyword::CommonLigatures:
            ligatures.common = Gfx::FontVariantLigatures::Common::Common;
            break;
        case Keyword::NoCommonLigatures:
            ligatures.common = Gfx::FontVariantLigatures::Common::NoCommon;
            break;
        case Keyword::DiscretionaryLigatures:
            ligatures.discretionary = Gfx::FontVariantLigatures::Discretionary::Discretionary;
            break;
        case Keyword::NoDiscretionaryLigatures:
            ligatures.discretionary = Gfx::FontVariantLigatures::Discretionary::NoDiscretionary;
            break;
        case Keyword::HistoricalLigatures:
            ligatures.historical = Gfx::FontVariantLigatures::Historical::Historical;
            break;
        case Keyword::NoHistoricalLigatures:
            ligatures.historical = Gfx::FontVariantLigatures::Historical::NoHistorical;
            break;
        case Keyword::Contextual:
            ligatures.contextual = Gfx::FontVariantLigatures::Contextual::Contextual;
            break;
        case Keyword::NoContextual:
            ligatures.contextual = Gfx::FontVariantLigatures::Contextual::NoContextual;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    if (value.is_keyword()) {
        apply_keyword(value.to_keyword());
    } else if (value.is_value_list()) {
        for (auto& child_value : value.as_value_list().values()) {
            apply_keyword(child_value->to_keyword());
        }
    }

    if (normal)
        return {};

    return ligatures;
}

Optional<Gfx::FontVariantNumeric> ComputedProperties::font_variant_numeric() const
{
    auto const& value = property(PropertyID::FontVariantNumeric);
    Gfx::FontVariantNumeric numeric {};
    bool normal = false;

    auto apply_keyword = [&numeric, &normal](Keyword keyword) {
        switch (keyword) {
        case Keyword::Normal:
            normal = true;
            break;
        case Keyword::Ordinal:
            numeric.ordinal = true;
            break;
        case Keyword::SlashedZero:
            numeric.slashed_zero = true;
            break;
        case Keyword::OldstyleNums:
            numeric.figure = Gfx::FontVariantNumeric::Figure::Oldstyle;
            break;
        case Keyword::LiningNums:
            numeric.figure = Gfx::FontVariantNumeric::Figure::Lining;
            break;
        case Keyword::ProportionalNums:
            numeric.spacing = Gfx::FontVariantNumeric::Spacing::Proportional;
            break;
        case Keyword::TabularNums:
            numeric.spacing = Gfx::FontVariantNumeric::Spacing::Tabular;
            break;
        case Keyword::DiagonalFractions:
            numeric.fraction = Gfx::FontVariantNumeric::Fraction::Diagonal;
            break;
        case Keyword::StackedFractions:
            numeric.fraction = Gfx::FontVariantNumeric::Fraction::Stacked;
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    if (value.is_keyword()) {
        apply_keyword(value.to_keyword());
    } else if (value.is_value_list()) {
        for (auto& child_value : value.as_value_list().values()) {
            apply_keyword(child_value->to_keyword());
        }
    }

    if (normal)
        return {};

    return numeric;
}

FontVariantPosition ComputedProperties::font_variant_position() const
{
    auto const& value = property(PropertyID::FontVariantPosition);
    return keyword_to_font_variant_position(value.to_keyword()).release_value();
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
                result.set(feature_tag.tag(), feature_tag.value()->as_integer().integer());
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
    auto const& value = property(PropertyID::FontVariationSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& axis_tags = value.as_value_list().values();
        HashMap<FlyString, NumberOrCalculated> result;
        result.ensure_capacity(axis_tags.size());
        for (auto const& tag_value : axis_tags) {
            auto const& axis_tag = tag_value->as_open_type_tagged();

            if (axis_tag.value()->is_number()) {
                result.set(axis_tag.tag(), axis_tag.value()->as_number().number());
            } else {
                VERIFY(axis_tag.value()->is_calculated());
                result.set(axis_tag.tag(), NumberOrCalculated { axis_tag.value()->as_calculated() });
            }
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

Vector<Vector<String>> ComputedProperties::grid_template_areas() const
{
    auto const& value = property(PropertyID::GridTemplateAreas);
    return value.as_grid_template_area().grid_template_area();
}

ObjectFit ComputedProperties::object_fit() const
{
    auto const& value = property(PropertyID::ObjectFit);
    return keyword_to_object_fit(value.to_keyword()).release_value();
}

ObjectPosition ComputedProperties::object_position() const
{
    auto const& value = property(PropertyID::ObjectPosition);
    auto const& position = value.as_position();
    ObjectPosition object_position;
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

MixBlendMode ComputedProperties::mix_blend_mode() const
{
    auto const& value = property(PropertyID::MixBlendMode);
    return keyword_to_mix_blend_mode(value.to_keyword()).release_value();
}

Optional<FlyString> ComputedProperties::view_transition_name() const
{
    auto const& value = property(PropertyID::ViewTransitionName);
    if (value.is_custom_ident())
        return value.as_custom_ident().custom_ident();
    return {};
}

MaskType ComputedProperties::mask_type() const
{
    auto const& value = property(PropertyID::MaskType);
    return keyword_to_mask_type(value.to_keyword()).release_value();
}

void ComputedProperties::set_math_depth(int math_depth)
{
    m_math_depth = math_depth;
    // Make our children inherit our computed value, not our specified value.
    set_property(PropertyID::MathDepth, MathDepthStyleValue::create_integer(IntegerStyleValue::create(math_depth)));
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
            if (counter.value) {
                if (counter.value->is_integer()) {
                    data.value = AK::clamp_to<i32>(counter.value->as_integer().integer());
                } else if (counter.value->is_calculated()) {
                    auto maybe_int = counter.value->as_calculated().resolve_integer({});
                    if (maybe_int.has_value())
                        data.value = AK::clamp_to<i32>(*maybe_int);
                } else {
                    dbgln("Unimplemented type for {} integer value: '{}'", string_from_property_id(property_id), counter.value->to_string(SerializationMode::Normal));
                }
            }
            result.append(move(data));
        }
        return result;
    }

    if (value.to_keyword() == Keyword::None)
        return {};

    dbgln("Unhandled type for {} value: '{}'", string_from_property_id(property_id), value.to_string(SerializationMode::Normal));
    return {};
}

ScrollbarColorData ComputedProperties::scrollbar_color(Layout::NodeWithStyle const& layout_node) const
{
    auto const& value = property(PropertyID::ScrollbarColor);
    if (value.is_keyword() && value.as_keyword().keyword() == Keyword::Auto)
        return InitialValues::scrollbar_color();

    if (value.is_scrollbar_color()) {
        auto& scrollbar_color_value = value.as_scrollbar_color();
        auto thumb_color = scrollbar_color_value.thumb_color()->to_color(ColorResolutionContext::for_layout_node_with_style(layout_node)).value();
        auto track_color = scrollbar_color_value.track_color()->to_color(ColorResolutionContext::for_layout_node_with_style(layout_node)).value();
        return { thumb_color, track_color };
    }

    return {};
}

ScrollbarWidth ComputedProperties::scrollbar_width() const
{
    auto const& value = property(PropertyID::ScrollbarWidth);
    return keyword_to_scrollbar_width(value.to_keyword()).release_value();
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

    if (value.is_value_list()) {
        auto const& value_list = value.as_value_list();
        Vector<WillChange::WillChangeEntry> will_change_entries;
        for (auto const& style_value : value_list.values()) {
            if (auto entry = to_will_change_entry(*style_value); entry.has_value())
                will_change_entries.append(*entry);
        }
        return WillChange(move(will_change_entries));
    }

    auto will_change_entry = to_will_change_entry(value);
    if (will_change_entry.has_value())
        return WillChange({ *will_change_entry });
    return WillChange::make_auto();
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

}
