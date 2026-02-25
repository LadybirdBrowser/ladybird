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
#include <LibWeb/Animations/AnimationTimeline.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/CSS/Clip.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/DOM/Document.h>
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

bool ComputedProperties::is_animated_property_result_of_transition(PropertyID property_id) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    return m_animated_property_result_of_transition[n / 8] & (1 << (n % 8));
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

void ComputedProperties::set_animated_property_result_of_transition(PropertyID property_id, AnimatedPropertyResultOfTransition animated_value_result_of_transition)
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    size_t n = to_underlying(property_id) - to_underlying(first_longhand_property_id);
    if (animated_value_result_of_transition == AnimatedPropertyResultOfTransition::Yes)
        m_animated_property_result_of_transition[n / 8] |= (1 << (n % 8));
    else
        m_animated_property_result_of_transition[n / 8] &= ~(1 << (n % 8));
}

void ComputedProperties::set_property(PropertyID id, NonnullRefPtr<StyleValue const> value, Inherited inherited, Important important)
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

void ComputedProperties::set_property_without_modifying_flags(PropertyID id, NonnullRefPtr<StyleValue const> value)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = move(value);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedProperties::revert_property(PropertyID id, ComputedProperties const& style_for_revert)
{
    VERIFY(id >= first_longhand_property_id && id <= last_longhand_property_id);

    m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)] = style_for_revert.m_property_values[to_underlying(id) - to_underlying(first_longhand_property_id)];
    set_property_important(id, style_for_revert.is_property_important(id) ? Important::Yes : Important::No);
    set_property_inherited(id, style_for_revert.is_property_inherited(id) ? Inherited::Yes : Inherited::No);
}

Display ComputedProperties::display_before_box_type_transformation() const
{
    return m_display_before_box_type_transformation;
}

void ComputedProperties::set_display_before_box_type_transformation(Display value)
{
    m_display_before_box_type_transformation = value;
}

void ComputedProperties::set_animated_property(PropertyID id, NonnullRefPtr<StyleValue const> value, AnimatedPropertyResultOfTransition animated_property_result_of_transition, Inherited inherited)
{
    m_animated_property_values.set(id, move(value));
    set_animated_property_inherited(id, inherited);
    set_animated_property_result_of_transition(id, animated_property_result_of_transition);

    if (property_affects_computed_font_list(id))
        clear_computed_font_list_cache();
}

void ComputedProperties::remove_animated_property(PropertyID id)
{
    m_animated_property_values.remove(id);
}

void ComputedProperties::reset_non_inherited_animated_properties(Badge<Animations::KeyframeEffect>)
{
    for (auto property_id : m_animated_property_values.keys()) {
        if (!is_animated_property_inherited(property_id))
            m_animated_property_values.remove(property_id);
    }
}

StyleValue const& ComputedProperties::property(PropertyID property_id, WithAnimationsApplied return_animated_value) const
{
    VERIFY(property_id >= first_longhand_property_id && property_id <= last_longhand_property_id);

    // Important properties override animated but not transitioned properties
    if ((!is_property_important(property_id) || is_animated_property_result_of_transition(property_id)) && return_animated_value == WithAnimationsApplied::Yes) {
        if (auto animated_value = m_animated_property_values.get(property_id); animated_value.has_value())
            return *animated_value.value();
    }

    // By the time we call this method, the property should have been assigned
    return *m_property_values[to_underlying(property_id) - to_underlying(first_longhand_property_id)];
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

Color ComputedProperties::color_or_fallback(PropertyID id, ColorResolutionContext color_resolution_context, Color fallback) const
{
    auto const& value = property(id);
    if (!value.has_color())
        return fallback;
    return value.to_color(color_resolution_context).value();
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

double ComputedProperties::stroke_miterlimit() const
{
    return number_from_style_value(property(PropertyID::StrokeMiterlimit), {});
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
    // FIXME: Support calc()
    if (!value.is_integer())
        return 0;
    return value.as_integer().integer();
}

ImageRendering ComputedProperties::image_rendering() const
{
    auto const& value = property(PropertyID::ImageRendering);
    return keyword_to_image_rendering(value.to_keyword()).release_value();
}

// https://drafts.csswg.org/css-backgrounds-4/#layering
Vector<BackgroundLayerData> ComputedProperties::background_layers() const
{
    auto const& background_image_values = property(PropertyID::BackgroundImage).as_value_list().values();

    // OPTIMIZATION: If all background-image values are `none`, we can skip computing the layers entirely
    if (all_of(background_image_values, [](auto const& value) { return value->to_keyword() == Keyword::None; }))
        return {};

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

        if (background_image_value->to_keyword() == Keyword::None)
            continue;

        auto const& background_attachment_value = coordinated_value_list.get(PropertyID::BackgroundAttachment)->at(i);
        auto const& background_blend_mode_value = coordinated_value_list.get(PropertyID::BackgroundBlendMode)->at(i);
        auto const& background_clip_value = coordinated_value_list.get(PropertyID::BackgroundClip)->at(i);
        auto const& background_origin_value = coordinated_value_list.get(PropertyID::BackgroundOrigin)->at(i);
        auto const& background_position_x_value = coordinated_value_list.get(PropertyID::BackgroundPositionX)->at(i);
        auto const& background_position_y_value = coordinated_value_list.get(PropertyID::BackgroundPositionY)->at(i);
        auto const& background_repeat_value = coordinated_value_list.get(PropertyID::BackgroundRepeat)->at(i);
        auto const& background_size_value = coordinated_value_list.get(PropertyID::BackgroundSize)->at(i);

        BackgroundLayerData layer {
            .background_image = background_image_value->as_abstract_image()
        };

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

Length ComputedProperties::border_spacing_horizontal() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 0);
        return Length::from_style_value(list.value_at(0, false), {});
    }

    return Length::from_style_value(style_value, {});
}

Length ComputedProperties::border_spacing_vertical() const
{
    auto const& style_value = property(PropertyID::BorderSpacing);

    if (style_value.is_value_list()) {
        auto const& list = style_value.as_value_list();
        VERIFY(list.size() > 1);
        return Length::from_style_value(list.value_at(1, false), {});
    }

    return Length::from_style_value(style_value, {});
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

Variant<Length, double> ComputedProperties::tab_size() const
{
    auto const& value = property(PropertyID::TabSize);
    if (value.is_calculated()) {
        auto const& math_value = value.as_calculated();
        if (math_value.resolves_to_length()) {
            return math_value.resolve_length({}).value();
        }
        if (math_value.resolves_to_number()) {
            return math_value.resolve_number({}).value();
        }
    }

    if (value.is_length())
        return value.as_length().length();

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

ListStyleType ComputedProperties::list_style_type(HashMap<FlyString, CounterStyle> const& registered_counter_styles) const
{
    auto const& value = property(PropertyID::ListStyleType);

    if (value.to_keyword() == Keyword::None)
        return Empty {};

    if (value.is_string())
        return value.as_string().string_value().to_string();

    return value.as_counter_style().resolve_counter_style(registered_counter_styles);
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

    auto make_shadow_data = [&layout_node](ShadowStyleValue const& value) -> Optional<ShadowData> {
        auto offset_x = Length::from_style_value(value.offset_x(), {});
        auto offset_y = Length::from_style_value(value.offset_y(), {});
        auto blur_radius = Length::from_style_value(value.blur_radius(), {});
        auto spread_distance = Length::from_style_value(value.spread_distance(), {});
        return ShadowData {
            offset_x,
            offset_y,
            blur_radius,
            spread_distance,
            value.color()->to_color(ColorResolutionContext::for_layout_node_with_style(as<Layout::NodeWithStyle>(layout_node))).value(),
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

Vector<ShadowData> ComputedProperties::box_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::BoxShadow, layout_node);
}

Vector<ShadowData> ComputedProperties::text_shadow(Layout::Node const& layout_node) const
{
    return shadow(PropertyID::TextShadow, layout_node);
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

Optional<FlyString> ComputedProperties::font_language_override() const
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

        if (value->is_font_variant_alternates_function()) {
            // FIXME: Support this
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

HashMap<FlyString, u8> ComputedProperties::font_feature_settings() const
{
    auto const& value = property(PropertyID::FontFeatureSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& feature_tags = value.as_value_list().values();
        HashMap<FlyString, u8> result;
        result.ensure_capacity(feature_tags.size());
        for (auto const& tag_value : feature_tags) {
            auto const& feature_tag = tag_value->as_open_type_tagged();

            result.set(feature_tag.tag(), int_from_style_value(feature_tag.value()));
        }
        return result;
    }

    return {};
}

HashMap<FlyString, double> ComputedProperties::font_variation_settings() const
{
    auto const& value = property(PropertyID::FontVariationSettings);

    if (value.is_keyword())
        return {}; // normal

    if (value.is_value_list()) {
        auto const& axis_tags = value.as_value_list().values();
        HashMap<FlyString, double> result;
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

Optional<FlyString> ComputedProperties::view_transition_name() const
{
    auto const& value = property(PropertyID::ViewTransitionName);
    if (value.is_custom_ident())
        return value.as_custom_ident().custom_ident();
    return {};
}

Vector<ComputedProperties::AnimationProperties> ComputedProperties::animations(DOM::AbstractElement const& abstract_element) const
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
        auto duration = [&] -> Variant<double, String> {
            // auto
            if (animation_duration_style_value->to_keyword() == Keyword::Auto) {
                // For time-driven animations, equivalent to 0s.
                return 0;

                // FIXME: For scroll-driven animations, equivalent to the duration necessary to fill the timeline in
                //        consideration of animation-range, animation-delay, and animation-iteration-count. See
                //        Scroll-driven Animations § 4.1 Finite Timeline Calculations.
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
        auto name = string_from_style_value(animation_name_style_value);

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
            if (animation_timeline_style_value->is_scroll_function()) {
                auto const& scroll_function = animation_timeline_style_value->as_scroll_function();

                Animations::ScrollTimeline::AnonymousSource source {
                    .scroller = scroll_function.scroller(),
                    .target = abstract_element,
                };

                return Animations::ScrollTimeline::create(abstract_element.element().realm(), abstract_element.document(), source, Animations::css_axis_to_bindings_scroll_axis(scroll_function.axis()));
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
        const_cast<ComputedProperties*>(this)->m_cached_computed_font_list = font_computer.compute_font_for_style_values(property(PropertyID::FontFamily), font_size(), font_slope(), font_weight(), font_width(), font_optical_sizing(), font_variation_settings(), font_feature_data());
        VERIFY(!m_cached_computed_font_list->is_empty());
    }

    return *m_cached_computed_font_list;
}

ValueComparingNonnullRefPtr<Gfx::Font const> ComputedProperties::first_available_computed_font(FontComputer const& font_computer) const
{
    if (!m_cached_first_available_computed_font) {
        // https://drafts.csswg.org/css-fonts/#first-available-font
        // First font for which the character U+0020 (space) is not excluded by a unicode-range
        const_cast<ComputedProperties*>(this)->m_cached_first_available_computed_font = computed_font_list(font_computer)->font_for_code_point(' ');
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
