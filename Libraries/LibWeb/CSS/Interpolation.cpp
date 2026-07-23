/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2025-2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Interpolation.h"
#include <AK/IntegralMath.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

template<typename T>
static T interpolate_raw(T from, T to, float delta, Optional<NumericRange> accepted_range = {})
{
    if constexpr (AK::Detail::IsSame<T, double>) {
        if (accepted_range.has_value())
            return clamp(from + (to - from) * static_cast<double>(delta), accepted_range->min, accepted_range->max);
        return from + (to - from) * static_cast<double>(delta);
    } else if constexpr (AK::Detail::IsIntegral<T>) {
        auto from_float = static_cast<float>(from);
        auto to_float = static_cast<float>(to);
        auto min = accepted_range.has_value() ? accepted_range->min : NumericLimits<T>::min();
        auto max = accepted_range.has_value() ? accepted_range->max : NumericLimits<T>::max();
        auto unclamped_result = roundf(from_float + (to_float - from_float) * delta);
        return static_cast<AK::Detail::RemoveCVReference<T>>(clamp(unclamped_result, min, max));
    }
    VERIFY(!accepted_range.has_value());
    return static_cast<AK::Detail::RemoveCVReference<T>>(from + (to - from) * delta);
}

static NonnullRefPtr<StyleValue const> with_keyword_values_resolved(DOM::Element& element, PropertyID property_id, StyleValue const& value)
{
    if (value.is_guaranteed_invalid()) {
        // At the moment, we're only dealing with "real" properties, so this behaves the same as `unset`.
        // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
        return property_initial_value(property_id);
    }

    if (!value.is_keyword())
        return value;
    switch (value.as_keyword().keyword()) {
    case Keyword::Initial:
    case Keyword::Unset:
        return property_initial_value(property_id);
    case Keyword::Inherit:
        return StyleComputer::get_non_animated_inherit_value(property_id, { element });
    default:
        break;
    }
    return value;
}

static RefPtr<StyleValue const> interpolate_discrete(StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    if (from.equals(to))
        return from;
    if (allow_discrete == AllowDiscrete::No)
        return {};
    return delta >= 0.5f ? to : from;
}

// https://drafts.fxtf.org/filter-effects/#interpolation-of-filter-functions
static RefPtr<FilterStyleValue const> interpolate_filter_function(DOM::Element& element, CalculationContext const& calculation_context, FilterStyleValue const& from, FilterStyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    VERIFY(!from.contains_url());
    VERIFY(!to.contains_url());

    if (from.kind() != to.kind())
        return {};

    switch (from.kind()) {
    case FilterStyleValue::Kind::Blur: {
        auto const& from_value = static_cast<BlurFilterStyleValue const&>(from);
        auto const& to_value = static_cast<BlurFilterStyleValue const&>(to);

        CalculationContext blur_calculation_context = calculation_context;
        blur_calculation_context.accepted_ranges_by_type.set(ValueType::Length, { 0, NumericLimits<float>::max() });
        if (auto interpolated_style_value = interpolate_value(element, blur_calculation_context, from_value.radius(), to_value.radius(), delta, allow_discrete))
            return BlurFilterStyleValue::create(interpolated_style_value.release_nonnull());
        return {};
    }
    case FilterStyleValue::Kind::HueRotate: {
        auto const& from_value = static_cast<HueRotateFilterStyleValue const&>(from);
        auto const& to_value = static_cast<HueRotateFilterStyleValue const&>(to);
        if (auto interpolated_style_value = interpolate_value(element, calculation_context, from_value.angle(), to_value.angle(), delta, allow_discrete))
            return HueRotateFilterStyleValue::create(interpolated_style_value.release_nonnull());
        return {};
    }
    case FilterStyleValue::Kind::Color: {
        auto const& from_value = static_cast<ColorFilterStyleValue const&>(from);
        auto const& to_value = static_cast<ColorFilterStyleValue const&>(to);
        if (from_value.operation() != to_value.operation())
            return {};
        auto operation = from_value.operation();

        CalculationContext filter_function_calculation_context = calculation_context;
        switch (operation) {
        case Gfx::ColorFilterType::Grayscale:
        case Gfx::ColorFilterType::Invert:
        case Gfx::ColorFilterType::Opacity:
        case Gfx::ColorFilterType::Sepia:
            filter_function_calculation_context.accepted_ranges_by_type.set(ValueType::Number, { 0, 1 });
            break;
        case Gfx::ColorFilterType::Brightness:
        case Gfx::ColorFilterType::Contrast:
        case Gfx::ColorFilterType::Saturate:
            filter_function_calculation_context.accepted_ranges_by_type.set(ValueType::Number, { 0, NumericLimits<float>::max() });
            break;
        }

        if (auto interpolated_style_value = interpolate_value(element, filter_function_calculation_context, from_value.amount(), to_value.amount(), delta, allow_discrete))
            return ColorFilterStyleValue::create(operation, *interpolated_style_value);
        return {};
    }
    case FilterStyleValue::Kind::DropShadow: {
        auto const& from_value = static_cast<DropShadowFilterStyleValue const&>(from);
        auto const& to_value = static_cast<DropShadowFilterStyleValue const&>(to);

        StyleValueVector from_shadows { from_value.shadow_style_value() };
        StyleValueVector to_shadows { to_value.shadow_style_value() };
        auto from_list = StyleValueList::create(move(from_shadows), StyleValueList::Separator::Comma);
        auto to_list = StyleValueList::create(move(to_shadows), StyleValueList::Separator::Comma);

        auto result = interpolate_box_shadow(element, calculation_context, *from_list, *to_list, delta, allow_discrete);
        if (!result)
            return {};

        auto const& result_shadow = result->as_value_list().value_at(0, false)->as_shadow();

        RefPtr<StyleValue const> result_radius;
        auto radius_has_value = delta >= 0.5f ? to_value.radius() : from_value.radius();
        if (radius_has_value)
            result_radius = result_shadow.blur_radius();

        return DropShadowFilterStyleValue::create(
            result_shadow.offset_x(),
            result_shadow.offset_y(),
            result_radius,
            result_shadow.color_or_null());
    }
    }
    VERIFY_NOT_REACHED();
}

static bool contains_url(StyleValueList const& list)
{
    return any_of(list.values(), [](auto& it) { return it->is_url(); });
}

// https://drafts.fxtf.org/filter-effects/#interpolation-of-filters
static RefPtr<StyleValue const> interpolate_filter_value_list(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete)
{
    auto is_interpolable_filter_list = [](StyleValue const& value) {
        if (!is_filter_style_value_list(value))
            return false;
        return !contains_url(value.as_value_list());
    };

    auto make_filter_value_list = [](StyleValueVector values) {
        return StyleValueList::create(move(values), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
    };

    auto interpolate_filter_values = [&](StyleValueList const& from, StyleValueList const& to) -> RefPtr<StyleValueList const> {
        StyleValueVector interpolated_filter_values;
        auto from_values = from.values();
        auto to_values = to.values();
        for (size_t i = 0; i < from.size(); ++i) {
            auto const& from_value = from_values[i]->as_filter();
            auto const& to_value = to_values[i]->as_filter();

            auto interpolated_value = interpolate_filter_function(element, calculation_context, from_value, to_value, delta, allow_discrete);
            if (!interpolated_value)
                return {};
            interpolated_filter_values.append(interpolated_value.release_nonnull());
        }
        return make_filter_value_list(move(interpolated_filter_values));
    };

    if (is_interpolable_filter_list(a_from) && is_interpolable_filter_list(a_to)) {
        auto const& from_list = a_from.as_value_list();
        auto const& to_list = a_to.as_value_list();
        // If both filters have a <filter-value-list> of same length without <url> and for each <filter-function> for which there is a corresponding item in each list
        if (from_list.size() == to_list.size()) {
            // Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
            return interpolate_filter_values(from_list, to_list);
        }

        // If both filters have a <filter-value-list> of different length without <url> and for each <filter-function> for which there is a corresponding item in each list

        // 1. Append the missing equivalent <filter-function>s from the longer list to the end of the shorter list. The new added <filter-function>s must be initialized to their initial values for interpolation.
        auto append_missing_values_to = [&](StyleValueList const& short_list, StyleValueList const& longer_list) -> ValueComparingNonnullRefPtr<StyleValueList> {
            StyleValueVector new_filter_list { short_list.values() };
            for (size_t i = new_filter_list.size(); i < longer_list.size(); ++i)
                new_filter_list.append(FilterStyleValue::initial_value_for(longer_list.values()[i]->as_filter(), true));
            return make_filter_value_list(move(new_filter_list));
        };
        ValueComparingNonnullRefPtr<StyleValue const> from = from_list.size() < to_list.size() ? append_missing_values_to(from_list, to_list) : a_from;
        ValueComparingNonnullRefPtr<StyleValue const> to = to_list.size() < from_list.size() ? append_missing_values_to(to_list, from_list) : a_to;

        // 2. Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
        return interpolate_filter_values(from->as_value_list(), to->as_value_list());
    }

    // If one filter is none and the other is a <filter-value-list> without <url>
    if ((is_interpolable_filter_list(a_from) && a_to.to_keyword() == Keyword::None)
        || (is_interpolable_filter_list(a_to) && a_from.to_keyword() == Keyword::None)) {

        // 1. Replace none with the corresponding <filter-value-list> of the other filter. The new <filter-function>s must be initialized to their initial values for interpolation.
        auto replace_none_with_initial_filter_list_values = [&](StyleValueList const& filter_value_list) {
            StyleValueVector initial_values;
            for (auto const& filter_value : filter_value_list.values()) {
                // FIXME: We shouldn't apply the default color here.
                initial_values.append(FilterStyleValue::initial_value_for(filter_value->as_filter(), true));
            }
            return make_filter_value_list(move(initial_values));
        };

        ValueComparingNonnullRefPtr<StyleValue const> from = a_from.is_keyword() ? replace_none_with_initial_filter_list_values(a_to.as_value_list()) : a_from;
        ValueComparingNonnullRefPtr<StyleValue const> to = a_to.is_keyword() ? replace_none_with_initial_filter_list_values(a_from.as_value_list()) : a_to;

        // 2. Interpolate each <filter-function> pair following the rules in section Interpolation of Filter Functions.
        return interpolate_filter_values(from->as_value_list(), to->as_value_list());
    }

    // Otherwise:
    // Use discrete interpolation
    return {};
}

struct ExpandedGridTracksAndLines {
    Vector<ExplicitGridTrack> tracks;
    Vector<Optional<GridLineNames>> line_names;
};

static ExpandedGridTracksAndLines expand_grid_tracks_and_lines(GridTrackSizeList const& list)
{
    ExpandedGridTracksAndLines result;
    Optional<ExplicitGridTrack> current_track;
    Optional<GridLineNames> current_line_names;
    auto append_result = [&] {
        result.tracks.append(*current_track);
        result.line_names.append(move(current_line_names));
        current_track.clear();
        current_line_names.clear();
    };

    for (auto const& component : list.list()) {
        if (auto const* grid_line_names = component.get_pointer<GridLineNames>()) {
            VERIFY(!current_line_names.has_value());
            current_line_names = *grid_line_names;
        } else if (auto const* grid_track = component.get_pointer<ExplicitGridTrack>()) {
            if (current_track.has_value())
                append_result();

            current_track = *grid_track;
        }
        if (current_track.has_value() && current_line_names.has_value())
            append_result();
    }
    if (current_track.has_value())
        append_result();

    return result;
}

static void append_grid_track_with_line_names(GridTrackSizeList& list, ExplicitGridTrack track, Optional<GridLineNames> line_names)
{
    list.append(move(track));
    if (line_names.has_value())
        list.append(line_names.release_value());
}

static Optional<GridTrackSizeList> interpolate_grid_track_size_list(DOM::Element& element, CalculationContext const& calculation_context, GridTrackSizeList const& from, GridTrackSizeList const& to, float delta)
{
    // https://drafts.csswg.org/css-grid-2/#track-sizing
    // Animation type: if the list lengths match, by computed value type per item in the computed track list;
    // discrete otherwise.
    //
    // https://drafts.csswg.org/css-grid-2/#computed-track-list-subgrid
    // The computed track list of a subgrid axis is the subgrid keyword followed by a list of line names.
    if (from.is_subgrid() || to.is_subgrid())
        return {};

    auto interpolate_grid_size = [&](GridSize const& from_grid_size, GridSize const& to_grid_size) -> GridSize {
        return GridSize { *interpolate_value(element, calculation_context, from_grid_size.style_value(), to_grid_size.style_value(), delta, AllowDiscrete::Yes) };
    };

    auto expanded_from = expand_grid_tracks_and_lines(from);
    auto expanded_to = expand_grid_tracks_and_lines(to);

    if (expanded_from.tracks.size() != expanded_to.tracks.size())
        return {};

    GridTrackSizeList result;
    for (size_t i = 0; i < expanded_from.tracks.size(); ++i) {
        auto& from_track = expanded_from.tracks[i];
        auto& to_track = expanded_to.tracks[i];
        auto interpolated_line_names = delta < 0.5f ? move(expanded_from.line_names[i]) : move(expanded_to.line_names[i]);

        if (from_track.is_repeat() || to_track.is_repeat()) {
            // https://drafts.csswg.org/css-grid/#repeat-interpolation
            if (!from_track.is_repeat() || !to_track.is_repeat())
                return {};

            auto from_repeat = from_track.repeat();
            auto to_repeat = to_track.repeat();
            if (!from_repeat.is_fixed() || !to_repeat.is_fixed())
                return {};
            if (from_repeat.repeat_count() != to_repeat.repeat_count() || from_repeat.grid_track_size_list().track_list().size() != to_repeat.grid_track_size_list().track_list().size())
                return {};

            auto interpolated_repeat_grid_tracks = interpolate_grid_track_size_list(element, calculation_context, from_repeat.grid_track_size_list(), to_repeat.grid_track_size_list(), delta);
            if (!interpolated_repeat_grid_tracks.has_value())
                return {};

            ExplicitGridTrack interpolated_grid_track { GridRepeat { from_repeat.type(), move(*interpolated_repeat_grid_tracks), IntegerStyleValue::create(from_repeat.repeat_count()) } };
            append_grid_track_with_line_names(result, move(interpolated_grid_track), move(interpolated_line_names));
        } else if (from_track.is_minmax() && to_track.is_minmax()) {
            auto from_minmax = from_track.minmax();
            auto to_minmax = to_track.minmax();
            auto interpolated_min = interpolate_grid_size(from_minmax.min_grid_size(), to_minmax.min_grid_size());
            auto interpolated_max = interpolate_grid_size(from_minmax.max_grid_size(), to_minmax.max_grid_size());
            ExplicitGridTrack interpolated_grid_track { GridMinMax { interpolated_min, interpolated_max } };
            append_grid_track_with_line_names(result, move(interpolated_grid_track), move(interpolated_line_names));
        } else if (from_track.is_default() && to_track.is_default()) {
            auto const& from_grid_size = from_track.grid_size();
            auto const& to_grid_size = to_track.grid_size();
            auto interpolated_grid_size = interpolate_grid_size(from_grid_size, to_grid_size);
            ExplicitGridTrack interpolated_grid_track { move(interpolated_grid_size) };
            append_grid_track_with_line_names(result, move(interpolated_grid_track), move(interpolated_line_names));
        } else {
            auto interpolated_grid_track = delta < 0.5f ? move(from_track) : move(to_track);
            append_grid_track_with_line_names(result, move(interpolated_grid_track), move(interpolated_line_names));
        }
    }
    return result;
}

ValueComparingRefPtr<StyleValue const> interpolate_property(DOM::Element& element, PropertyID property_id, StyleValue const& a_from, StyleValue const& a_to, float delta, AllowDiscrete allow_discrete, ColorResolutionContext const* color_resolution_context)
{
    auto from = with_keyword_values_resolved(element, property_id, a_from);
    auto to = with_keyword_values_resolved(element, property_id, a_to);

    StyleValueFFI::FfiAnimationContext animation_context {
        .allow_discrete = allow_discrete == AllowDiscrete::Yes,
        .has_transform_reference_box = false,
        .transform_reference_box_width = 0,
        .transform_reference_box_height = 0,
    };
    if (property_id == PropertyID::Transform) {
        if (auto paintable = element.unsafe_paintable(); paintable) {
            auto reference_box = paintable->transform_reference_box();
            animation_context.has_transform_reference_box = true;
            animation_context.transform_reference_box_width = reference_box.width().to_double();
            animation_context.transform_reference_box_height = reference_box.height().to_double();
        }
    }
    auto rust_result = StyleValueFFI::rust_interpolate_scalar_style_value(&animation_context, to_underlying(property_id), from->rust_style_value_data(), to->rust_style_value_data(), delta);
    if (rust_result.handled) {
        if (!rust_result.value)
            return {};
        return StyleValue::adopt_rust_style_value_data(rust_result.value);
    }

    StyleValueFFI::rust_style_ffi_note_animation_cpp_interpolation_fallback();

    auto calculation_context = CalculationContext::for_property(PropertyNameAndID::from_id(property_id));

    auto animation_type = animation_type_from_longhand_property(property_id);
    switch (animation_type) {
    case AnimationType::ByComputedValue:
        return interpolate_value(element, calculation_context, from, to, delta, allow_discrete, color_resolution_context);
    case AnimationType::None:
        return to;
    case AnimationType::RepeatableList:
        return interpolate_repeatable_list(element, calculation_context, from, to, delta, allow_discrete, color_resolution_context);
    case AnimationType::Custom: {
        if (property_id == PropertyID::BoxShadow || property_id == PropertyID::TextShadow) {
            if (auto interpolated_box_shadow = interpolate_box_shadow(element, calculation_context, from, to, delta, allow_discrete))
                return *interpolated_box_shadow;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::Filter || property_id == PropertyID::BackdropFilter) {
            if (auto result = interpolate_filter_value_list(element, calculation_context, from, to, delta, allow_discrete))
                return result;
            return interpolate_discrete(from, to, delta, allow_discrete);
        }

        if (property_id == PropertyID::GridTemplateRows || property_id == PropertyID::GridTemplateColumns) {
            // https://drafts.csswg.org/css-grid/#track-sizing
            // If the list lengths match, by computed value type per item in the computed track list.
            auto from_list = from->as_grid_track_size_list().grid_track_size_list();
            auto to_list = to->as_grid_track_size_list().grid_track_size_list();

            auto interpolated_grid_tack_size_list = interpolate_grid_track_size_list(element, calculation_context, from_list, to_list, delta);
            if (!interpolated_grid_tack_size_list.has_value())
                return interpolate_discrete(from, to, delta, allow_discrete);

            return GridTrackSizeListStyleValue::create(interpolated_grid_tack_size_list.release_value());
        }

        // FIXME: Handle all custom animatable properties
        [[fallthrough]];
    }
    case AnimationType::Discrete:
    default:
        return interpolate_discrete(from, to, delta, allow_discrete);
    }
}

// https://drafts.csswg.org/css-transitions/#transitionable
bool property_values_are_transitionable(PropertyID property_id, StyleValue const& old_value, StyleValue const& new_value, DOM::Element& element, TransitionBehavior transition_behavior)
{
    // When comparing the before-change style and after-change style for a given property,
    // the property values are transitionable if they have an animation type that is neither not animatable nor discrete.

    auto animation_type = animation_type_from_longhand_property(property_id);
    if (animation_type == AnimationType::None || (transition_behavior != TransitionBehavior::AllowDiscrete && animation_type == AnimationType::Discrete))
        return false;

    // Even when a property is transitionable, the two values may not be. The spec uses the example of inset/non-inset shadows.
    if (transition_behavior != TransitionBehavior::AllowDiscrete && !interpolate_property(element, property_id, old_value, new_value, 0.5f, AllowDiscrete::No))
        return false;

    return true;
}

RefPtr<StyleValue const> interpolate_box_shadow(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete)
{
    // https://drafts.csswg.org/css-backgrounds/#box-shadow
    // Animation type: by computed value, treating none as a zero-item list and appending blank shadows
    //                 (transparent 0 0 0 0) with a corresponding inset keyword as needed to match the longer list if
    //                 the shorter list is otherwise compatible with the longer one

    static constexpr auto process_list = [](StyleValue const& value) -> StyleValueVector {
        if (value.to_keyword() == Keyword::None)
            return {};

        return StyleValueVector { value.as_value_list().values() };
    };

    static constexpr auto extend_list_if_necessary = [](StyleValueVector& values, StyleValueVector const& other) {
        values.ensure_capacity(other.size());
        for (size_t i = values.size(); i < other.size(); i++) {
            values.unchecked_append(ShadowStyleValue::create(
                other.get(0).value()->as_shadow().shadow_type(),
                ColorStyleValue::create_from_color(Color::Transparent, ColorSyntax::Legacy),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                LengthStyleValue::create(Length::make_px(0)),
                other[i]->as_shadow().placement()));
        }
    };

    StyleValueVector from_shadows = process_list(from);
    StyleValueVector to_shadows = process_list(to);

    extend_list_if_necessary(from_shadows, to_shadows);
    extend_list_if_necessary(to_shadows, from_shadows);

    VERIFY(from_shadows.size() == to_shadows.size());
    StyleValueVector result_shadows;
    result_shadows.ensure_capacity(from_shadows.size());

    // NB: Called during style interpolation.
    ColorResolutionContext color_resolution_context {};
    if (auto* node = element.unsafe_layout_node())
        color_resolution_context = ColorResolutionContext::for_layout_node_with_style(*node);

    for (size_t i = 0; i < from_shadows.size(); i++) {
        auto const& from_shadow = from_shadows[i]->as_shadow();
        auto const& to_shadow = to_shadows[i]->as_shadow();
        auto interpolated_offset_x = interpolate_value(element, calculation_context, from_shadow.offset_x(), to_shadow.offset_x(), delta, allow_discrete);
        auto interpolated_offset_y = interpolate_value(element, calculation_context, from_shadow.offset_y(), to_shadow.offset_y(), delta, allow_discrete);
        auto interpolated_blur_radius = interpolate_value(element, calculation_context, from_shadow.blur_radius(), to_shadow.blur_radius(), delta, allow_discrete);
        auto interpolated_spread_distance = interpolate_value(element, calculation_context, from_shadow.spread_distance(), to_shadow.spread_distance(), delta, allow_discrete);
        if (!interpolated_offset_x || !interpolated_offset_y || !interpolated_blur_radius || !interpolated_spread_distance)
            return {};

        auto interpolated_color_value = interpolate_color(*from_shadow.color(), *to_shadow.color(), delta, {}, color_resolution_context);
        if (!interpolated_color_value)
            interpolated_color_value = ColorStyleValue::create_from_color(Color::Black, ColorSyntax::Modern);

        auto result_shadow = ShadowStyleValue::create(
            from_shadow.shadow_type(),
            interpolated_color_value.release_nonnull(),
            *interpolated_offset_x,
            *interpolated_offset_y,
            *interpolated_blur_radius,
            *interpolated_spread_distance,
            delta >= 0.5f ? to_shadow.placement() : from_shadow.placement());
        result_shadows.unchecked_append(result_shadow);
    }

    return StyleValueList::create(move(result_shadows), StyleValueList::Separator::Comma);
}

static Optional<ValueType> get_value_type_of_numeric_style_value(StyleValue const& value, CalculationContext const& calculation_context)
{
    switch (value.type()) {
    case StyleValue::Type::Angle:
        return ValueType::Angle;
    case StyleValue::Type::Frequency:
        return ValueType::Frequency;
    case StyleValue::Type::Integer:
        return ValueType::Integer;
    case StyleValue::Type::Length:
        return ValueType::Length;
    case StyleValue::Type::Number:
        return ValueType::Number;
    case StyleValue::Type::Percentage:
        return calculation_context.percentages_resolve_as.value_or(ValueType::Percentage);
    case StyleValue::Type::Resolution:
        return ValueType::Resolution;
    case StyleValue::Type::Time:
        return ValueType::Time;
    case StyleValue::Type::Calculated: {
        auto const& calculated = value.as_calculated();
        if (calculated.resolves_to_angle_percentage())
            return ValueType::Angle;
        if (calculated.resolves_to_frequency_percentage())
            return ValueType::Frequency;
        if (calculated.resolves_to_length_percentage())
            return ValueType::Length;
        if (calculated.resolves_to_resolution())
            return ValueType::Resolution;
        if (calculated.resolves_to_number())
            return calculation_context.resolve_numbers_as_integers ? ValueType::Integer : ValueType::Number;
        if (calculated.resolves_to_percentage())
            return calculation_context.percentages_resolve_as.value_or(ValueType::Percentage);
        if (calculated.resolves_to_time_percentage())
            return ValueType::Time;

        return {};
    }
    default:
        return {};
    }
}

static RefPtr<StyleValue const> interpolate_mixed_value(CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta)
{
    auto from_value_type = get_value_type_of_numeric_style_value(from, calculation_context);
    auto to_value_type = get_value_type_of_numeric_style_value(to, calculation_context);

    if (from_value_type.has_value() && from_value_type == to_value_type) {
        // https://drafts.csswg.org/css-values-4/#combine-mixed
        // The computed value of a percentage-dimension mix is defined as
        // FIXME: a computed dimension if the percentage component is zero or is defined specifically to compute to a dimension value
        // a computed percentage if the dimension component is zero
        // a computed calc() expression otherwise
        if (auto const* from_dimension_value = as_if<DimensionStyleValue>(from); from_dimension_value && to.type() == StyleValue::Type::Percentage) {
            auto dimension_component = from_dimension_value->raw_value() * (1.f - delta);
            auto percentage_component = to.as_percentage().raw_value() * delta;
            if (dimension_component == 0.f)
                return PercentageStyleValue::create(Percentage { percentage_component });
        } else if (auto const* to_dimension_value = as_if<DimensionStyleValue>(to); to_dimension_value && from.type() == StyleValue::Type::Percentage) {
            auto dimension_component = to_dimension_value->raw_value() * delta;
            auto percentage_component = from.as_percentage().raw_value() * (1.f - delta);
            if (dimension_component == 0)
                return PercentageStyleValue::create(Percentage { percentage_component });
        }

        // https://drafts.csswg.org/css-values-4/#combine-math
        // Interpolation of math functions, with each other or with numeric values and other numeric-valued functions, is defined as Vresult = calc((1 - p) * VA + p * VB).
        Vector<CalcNodeRef> from_contribution_factors;
        from_contribution_factors.append(CalcNodeRef::from_style_value(from));
        from_contribution_factors.append(CalcNodeRef::numeric(Number { Number::Type::Number, 1.f - delta }));
        auto from_contribution = CalcNodeRef::product(move(from_contribution_factors));

        Vector<CalcNodeRef> to_contribution_factors;
        to_contribution_factors.append(CalcNodeRef::from_style_value(to));
        to_contribution_factors.append(CalcNodeRef::numeric(Number { Number::Type::Number, delta }));
        auto to_contribution = CalcNodeRef::product(move(to_contribution_factors));

        Vector<CalcNodeRef> contributions;
        contributions.append(move(from_contribution));
        contributions.append(move(to_contribution));
        auto interpolated_sum = CalcNodeRef::sum(move(contributions));

        auto numeric_type = interpolated_sum.determine_type(calculation_context);
        return CalculatedStyleValue::create(
            simplify_a_calculation_tree(interpolated_sum, calculation_context, {}),
            numeric_type.value(),
            calculation_context);
    }

    return {};
}

static RefPtr<StyleValue const> interpolate_value_impl(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete, ColorResolutionContext const* color_resolution_context)
{
    ColorResolutionContext fallback_color_resolution_context {};
    if (from.has_color() && to.has_color()) {
        if (!color_resolution_context) {
            if (auto* node = element.unsafe_layout_node())
                fallback_color_resolution_context = ColorResolutionContext::for_layout_node_with_style(*node);
            color_resolution_context = &fallback_color_resolution_context;
        }
        if (auto interpolated = interpolate_color(from, to, delta, {}, *color_resolution_context))
            return interpolated;
        if (from.type() == StyleValue::Type::Color && to.type() == StyleValue::Type::Color)
            return ColorStyleValue::create_from_color(Color::Black, ColorSyntax::Modern);
    }

    if (from.type() != to.type() || from.is_calculated() || to.is_calculated()) {
        // Handle mixed percentage and dimension types, as well as CalculatedStyleValues
        // https://www.w3.org/TR/css-values-4/#mixed-percentages
        return interpolate_mixed_value(calculation_context, from, to, delta);
    }

    switch (from.type()) {
    case StyleValue::Type::Angle: {
        auto interpolated_value = interpolate_raw(from.as_angle().angle().to_degrees(), to.as_angle().angle().to_degrees(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Angle));
        return AngleStyleValue::create(Angle::make_degrees(interpolated_value));
    }
    case StyleValue::Type::BackgroundSize: {
        auto interpolated_x = interpolate_value(element, calculation_context, from.as_background_size().size_x(), to.as_background_size().size_x(), delta, allow_discrete);
        auto interpolated_y = interpolate_value(element, calculation_context, from.as_background_size().size_y(), to.as_background_size().size_y(), delta, allow_discrete);
        if (!interpolated_x || !interpolated_y)
            return {};

        return BackgroundSizeStyleValue::create(*interpolated_x, *interpolated_y);
    }
    case StyleValue::Type::BorderImageSlice: {
        auto& from_border_image_slice = from.as_border_image_slice();
        auto& to_border_image_slice = to.as_border_image_slice();
        if (from_border_image_slice.fill() != to_border_image_slice.fill())
            return {};
        auto interpolated_top = interpolate_value(element, calculation_context, from_border_image_slice.top(), to_border_image_slice.top(), delta, allow_discrete);
        auto interpolated_right = interpolate_value(element, calculation_context, from_border_image_slice.right(), to_border_image_slice.right(), delta, allow_discrete);
        auto interpolated_bottom = interpolate_value(element, calculation_context, from_border_image_slice.bottom(), to_border_image_slice.bottom(), delta, allow_discrete);
        auto interpolated_left = interpolate_value(element, calculation_context, from_border_image_slice.left(), to_border_image_slice.left(), delta, allow_discrete);
        if (!interpolated_top || !interpolated_right || !interpolated_bottom || !interpolated_left)
            return {};
        return BorderImageSliceStyleValue::create(
            interpolated_top.release_nonnull(),
            interpolated_right.release_nonnull(),
            interpolated_bottom.release_nonnull(),
            interpolated_left.release_nonnull(),
            from_border_image_slice.fill());
    }
    case StyleValue::Type::BasicShape: {
        // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
        auto& from_shape = from.as_basic_shape().basic_shape();
        auto& to_shape = to.as_basic_shape().basic_shape();
        if (from_shape.index() != to_shape.index())
            return {};

        CalculationContext basic_shape_calculation_context {
            .percentages_resolve_as = ValueType::Length
        };

        auto const interpolate_optional_position = [&](RefPtr<StyleValue const> from_position, RefPtr<StyleValue const> to_position) -> Optional<RefPtr<StyleValue const>> {
            if (!from_position && !to_position)
                return nullptr;

            auto const& from_position_with_default = from_position ? from_position.release_nonnull() : PositionStyleValue::create_computed_center();
            auto const& to_position_with_default = to_position ? to_position.release_nonnull() : PositionStyleValue::create_computed_center();

            auto interpolated_position = interpolate_value(element, basic_shape_calculation_context, from_position_with_default, to_position_with_default, delta, allow_discrete);

            // NB: Use OptionalNone to indicate failure to interpolate since nullptr is a valid result for interpolating
            //     between two null positions.
            if (!interpolated_position)
                return OptionalNone {};

            return interpolated_position;
        };

        auto interpolated_shape = from_shape.visit(
            [&](Inset const& from_inset) -> Optional<BasicShape> {
                // If both shapes are of type inset(), interpolate between each value in the shape functions.
                auto& to_inset = to_shape.get<Inset>();
                auto interpolated_top = interpolate_value(element, basic_shape_calculation_context, from_inset.top, to_inset.top, delta, allow_discrete);
                auto interpolated_right = interpolate_value(element, basic_shape_calculation_context, from_inset.right, to_inset.right, delta, allow_discrete);
                auto interpolated_bottom = interpolate_value(element, basic_shape_calculation_context, from_inset.bottom, to_inset.bottom, delta, allow_discrete);
                auto interpolated_left = interpolate_value(element, basic_shape_calculation_context, from_inset.left, to_inset.left, delta, allow_discrete);

                auto interpolated_border_radius = interpolate_value(element, basic_shape_calculation_context, from_inset.border_radius, to_inset.border_radius, delta, allow_discrete);

                if (!interpolated_top || !interpolated_right || !interpolated_bottom || !interpolated_left || !interpolated_border_radius)
                    return {};

                return Inset { interpolated_top.release_nonnull(), interpolated_right.release_nonnull(), interpolated_bottom.release_nonnull(), interpolated_left.release_nonnull(), interpolated_border_radius.release_nonnull() };
            },
            [&](Circle const& from_circle) -> Optional<BasicShape> {
                // If both shapes are the same type, that type is ellipse() or circle(), and the radiuses are specified
                // as <length-percentage> (rather than keywords), interpolate between each value in the shape functions.
                auto const& to_circle = to_shape.get<Circle>();
                auto interpolated_radius = interpolate_value_impl(element, basic_shape_calculation_context, from_circle.radius, to_circle.radius, delta, AllowDiscrete::No, color_resolution_context);
                auto interpolated_position = interpolate_optional_position(from_circle.position, to_circle.position);
                if (!interpolated_radius || !interpolated_position.has_value())
                    return {};

                return Circle { interpolated_radius.release_nonnull(), interpolated_position.value() };
            },
            [&](Ellipse const& from_ellipse) -> Optional<BasicShape> {
                auto const& to_ellipse = to_shape.get<Ellipse>();
                auto interpolated_radius = interpolate_value_impl(element, basic_shape_calculation_context, from_ellipse.radius, to_ellipse.radius, delta, AllowDiscrete::No, color_resolution_context);
                auto interpolated_position = interpolate_optional_position(from_ellipse.position, to_ellipse.position);
                if (!interpolated_radius || !interpolated_position.has_value())
                    return {};

                return Ellipse { interpolated_radius.release_nonnull(), interpolated_position.value() };
            },
            [&](Polygon const& from_polygon) -> Optional<BasicShape> {
                // If both shapes are of type polygon(), both polygons have the same number of vertices, and use the
                // same <'fill-rule'>, interpolate between each value in the shape functions.
                auto const& to_polygon = to_shape.get<Polygon>();
                if (from_polygon.fill_rule != to_polygon.fill_rule)
                    return {};
                if (from_polygon.points.size() != to_polygon.points.size())
                    return {};
                Vector<Polygon::Point> interpolated_points;
                interpolated_points.ensure_capacity(from_polygon.points.size());
                for (size_t i = 0; i < from_polygon.points.size(); i++) {
                    auto const& from_point = from_polygon.points[i];
                    auto const& to_point = to_polygon.points[i];
                    auto interpolated_point_x = interpolate_value(element, basic_shape_calculation_context, from_point.x, to_point.x, delta, allow_discrete);
                    auto interpolated_point_y = interpolate_value(element, basic_shape_calculation_context, from_point.y, to_point.y, delta, allow_discrete);
                    if (!interpolated_point_x || !interpolated_point_y)
                        return {};
                    interpolated_points.unchecked_append(Polygon::Point { *interpolated_point_x, *interpolated_point_y });
                }

                return Polygon { from_polygon.fill_rule, move(interpolated_points) };
            },
            [](auto&) -> Optional<BasicShape> {
                return {};
            });

        if (!interpolated_shape.has_value())
            return {};

        return BasicShapeStyleValue::create(*interpolated_shape);
    }
    case StyleValue::Type::BorderRadius: {
        auto const& from_horizontal_radius = from.as_border_radius().horizontal_radius();
        auto const& to_horizontal_radius = to.as_border_radius().horizontal_radius();
        auto const& from_vertical_radius = from.as_border_radius().vertical_radius();
        auto const& to_vertical_radius = to.as_border_radius().vertical_radius();
        auto interpolated_horizontal_radius = interpolate_value_impl(element, calculation_context, from_horizontal_radius, to_horizontal_radius, delta, allow_discrete, color_resolution_context);
        auto interpolated_vertical_radius = interpolate_value_impl(element, calculation_context, from_vertical_radius, to_vertical_radius, delta, allow_discrete, color_resolution_context);
        if (!interpolated_horizontal_radius || !interpolated_vertical_radius)
            return {};
        return BorderRadiusStyleValue::create(interpolated_horizontal_radius.release_nonnull(), interpolated_vertical_radius.release_nonnull());
    }
    case StyleValue::Type::BorderRadiusRect: {
        CalculationContext border_radius_rect_computation_context = {
            .percentages_resolve_as = ValueType::Length,
            .accepted_ranges_by_type = { { ValueType::Length, { 0, AK::NumericLimits<float>::max() } }, { ValueType::Percentage, { 0, AK::NumericLimits<float>::max() } } },
        };

        auto const& from_top_left = from.as_border_radius_rect().top_left();
        auto const& to_top_left = to.as_border_radius_rect().top_left();

        auto const& from_top_right = from.as_border_radius_rect().top_right();
        auto const& to_top_right = to.as_border_radius_rect().top_right();

        auto const& from_bottom_right = from.as_border_radius_rect().bottom_right();
        auto const& to_bottom_right = to.as_border_radius_rect().bottom_right();

        auto const& from_bottom_left = from.as_border_radius_rect().bottom_left();
        auto const& to_bottom_left = to.as_border_radius_rect().bottom_left();

        auto interpolated_top_left = interpolate_value_impl(element, border_radius_rect_computation_context, from_top_left, to_top_left, delta, allow_discrete, color_resolution_context);
        auto interpolated_top_right = interpolate_value_impl(element, border_radius_rect_computation_context, from_top_right, to_top_right, delta, allow_discrete, color_resolution_context);
        auto interpolated_bottom_right = interpolate_value_impl(element, border_radius_rect_computation_context, from_bottom_right, to_bottom_right, delta, allow_discrete, color_resolution_context);
        auto interpolated_bottom_left = interpolate_value_impl(element, border_radius_rect_computation_context, from_bottom_left, to_bottom_left, delta, allow_discrete, color_resolution_context);

        if (!interpolated_top_left || !interpolated_top_right || !interpolated_bottom_right || !interpolated_bottom_left)
            return {};

        return BorderRadiusRectStyleValue::create(interpolated_top_left.release_nonnull(), interpolated_top_right.release_nonnull(), interpolated_bottom_right.release_nonnull(), interpolated_bottom_left.release_nonnull());
    }
    case StyleValue::Type::Color:
        VERIFY_NOT_REACHED();
    case StyleValue::Type::Edge: {
        auto const& from_offset = from.as_edge().offset();
        auto const& to_offset = to.as_edge().offset();

        if (auto interpolated_value = interpolate_value_impl(element, calculation_context, from_offset, to_offset, delta, allow_discrete, color_resolution_context))
            return EdgeStyleValue::create({}, interpolated_value);

        return {};
    }
    case StyleValue::Type::FontStyle: {
        auto const& from_font_style = from.as_font_style();
        auto const& to_font_style = to.as_font_style();
        auto interpolated_font_style = interpolate_value(element, calculation_context, KeywordStyleValue::create(to_keyword(from_font_style.font_style())), KeywordStyleValue::create(to_keyword(to_font_style.font_style())), delta, allow_discrete);
        if (!interpolated_font_style)
            return {};
        if (from_font_style.angle() && to_font_style.angle()) {
            auto interpolated_angle = interpolate_value(element, { .accepted_ranges_by_type = { { ValueType::Angle, { -90, 90 } } } }, *from_font_style.angle(), *to_font_style.angle(), delta, allow_discrete);
            if (!interpolated_angle)
                return {};
            return FontStyleStyleValue::create(*keyword_to_font_style_keyword(interpolated_font_style->to_keyword()), interpolated_angle);
        }

        return FontStyleStyleValue::create(*keyword_to_font_style_keyword(interpolated_font_style->to_keyword()));
    }
    case StyleValue::Type::Flex: {
        auto interpolated_value = interpolate_raw(from.as_flex().flex().to_fr(), to.as_flex().flex().to_fr(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Flex));
        return FlexStyleValue::create(Flex::make_fr(interpolated_value));
    }
    case StyleValue::Type::Function: {
        auto const& from_function = from.as_function();
        auto const& to_function = to.as_function();

        if (from_function.name() != to_function.name())
            return {};

        auto interpolated_value = interpolate_value(element, calculation_context, from_function.value(), to_function.value(), delta, allow_discrete);
        if (!interpolated_value)
            return {};

        return FunctionStyleValue::create(from_function.name(), interpolated_value.release_nonnull());
    }
    case StyleValue::Type::Integer: {
        // https://drafts.csswg.org/css-values/#combine-integers
        // Interpolation of <integer> is defined as Vresult = round((1 - p) × VA + p × VB);
        // that is, interpolation happens in the real number space as for <number>s, and the result is converted to an <integer> by rounding to the nearest integer.
        auto interpolated_value = interpolate_raw(from.as_integer().integer(), to.as_integer().integer(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Integer));
        return IntegerStyleValue::create(interpolated_value);
    }
    case StyleValue::Type::Length: {
        auto const& from_length = from.as_length().length();
        auto const& to_length = to.as_length().length();
        auto interpolated_value = interpolate_raw(from_length.raw_value(), to_length.raw_value(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Length));
        return LengthStyleValue::create(Length(interpolated_value, from_length.unit()));
    }
    case StyleValue::Type::Number: {
        auto interpolated_value = interpolate_raw(from.as_number().number(), to.as_number().number(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Number));
        return NumberStyleValue::create(interpolated_value);
    }
    case StyleValue::Type::OpacityValue: {
        auto interpolated_value = interpolate_raw(from.as_opacity_value().resolved(), to.as_opacity_value().resolved(), delta, NumericRange { .min = 0, .max = 1 });
        return OpacityValueStyleValue::create(NumberStyleValue::create(interpolated_value));
    }
    case StyleValue::Type::OpenTypeTagged: {
        auto& from_open_type_tagged = from.as_open_type_tagged();
        auto& to_open_type_tagged = to.as_open_type_tagged();
        if (from_open_type_tagged.tag() != to_open_type_tagged.tag())
            return {};
        auto interpolated_value = interpolate_value(element, calculation_context, from_open_type_tagged.value(), to_open_type_tagged.value(), delta, allow_discrete);
        if (!interpolated_value)
            return {};
        return OpenTypeTaggedStyleValue::create(OpenTypeTaggedStyleValue::Mode::FontVariationSettings, from_open_type_tagged.tag(), interpolated_value.release_nonnull());
    }
    case StyleValue::Type::Percentage: {
        auto interpolated_value = interpolate_raw(from.as_percentage().percentage().value(), to.as_percentage().percentage().value(), delta, calculation_context.accepted_ranges_by_type.get(ValueType::Percentage));
        return PercentageStyleValue::create(Percentage(interpolated_value));
    }
    case StyleValue::Type::Position: {
        // https://www.w3.org/TR/css-values-4/#combine-positions
        // FIXME: Interpolation of <position> is defined as the independent interpolation of each component (x, y) normalized as an offset from the top left corner as a <length-percentage>.
        auto const& from_position = from.as_position();
        auto const& to_position = to.as_position();
        auto interpolated_edge_x = interpolate_value(element, calculation_context, from_position.edge_x(), to_position.edge_x(), delta, allow_discrete);
        auto interpolated_edge_y = interpolate_value(element, calculation_context, from_position.edge_y(), to_position.edge_y(), delta, allow_discrete);
        if (!interpolated_edge_x || !interpolated_edge_y)
            return {};
        return PositionStyleValue::create(interpolated_edge_x->as_edge(), interpolated_edge_y->as_edge());
    }
    case StyleValue::Type::RadialSize: {
        auto const& from_components = from.as_radial_size().components();
        auto const& to_components = to.as_radial_size().components();

        auto const is_radial_extent = [](auto const& component) { return component.template has<RadialExtent>(); };

        // https://drafts.csswg.org/css-images-4/#interpolating-gradients
        // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
        // FIXME: Radial extents should disallow interpolation for basic-shape values but should be converted into their
        //        equivalent length-percentage values for radial gradients
        if (any_of(from_components, is_radial_extent) || any_of(to_components, is_radial_extent))
            return {};

        CalculationContext radial_size_calculation_context {
            .percentages_resolve_as = ValueType::Length,
            .accepted_ranges_by_type = {
                { ValueType::Length, { 0, AK::NumericLimits<float>::max() } },
            }
        };

        if (from_components.size() == 1 && to_components.size() == 1) {
            auto const& from_component = from_components[0].get<NonnullRefPtr<StyleValue const>>();
            auto const& to_component = to_components[0].get<NonnullRefPtr<StyleValue const>>();

            auto interpolated_value = interpolate_value(element, radial_size_calculation_context, from_component, to_component, delta, allow_discrete);

            if (!interpolated_value)
                return {};

            return RadialSizeStyleValue::create({ interpolated_value.release_nonnull() });
        }

        auto const& from_horizontal_component = from_components[0].get<NonnullRefPtr<StyleValue const>>();
        auto const& from_vertical_component = from_components.size() > 1 ? from_components[1].get<NonnullRefPtr<StyleValue const>>() : from_horizontal_component;

        auto const& to_horizontal_component = to_components[0].get<NonnullRefPtr<StyleValue const>>();
        auto const& to_vertical_component = to_components.size() > 1 ? to_components[1].get<NonnullRefPtr<StyleValue const>>() : to_horizontal_component;

        auto interpolated_horizontal = interpolate_value(element, radial_size_calculation_context, from_horizontal_component, to_horizontal_component, delta, allow_discrete);
        auto interpolated_vertical = interpolate_value(element, radial_size_calculation_context, from_vertical_component, to_vertical_component, delta, allow_discrete);

        if (!interpolated_horizontal || !interpolated_vertical)
            return {};

        return RadialSizeStyleValue::create({ interpolated_horizontal.release_nonnull(), interpolated_vertical.release_nonnull() });
    }
    case StyleValue::Type::Rect: {
        auto const& from_rect = from.as_rect();
        auto const& to_rect = to.as_rect();

        auto interpolated_top = interpolate_value_impl(element, calculation_context, from_rect.top(), to_rect.top(), delta, allow_discrete, color_resolution_context);
        auto interpolated_right = interpolate_value_impl(element, calculation_context, from_rect.right(), to_rect.right(), delta, allow_discrete, color_resolution_context);
        auto interpolated_bottom = interpolate_value_impl(element, calculation_context, from_rect.bottom(), to_rect.bottom(), delta, allow_discrete, color_resolution_context);
        auto interpolated_left = interpolate_value_impl(element, calculation_context, from_rect.left(), to_rect.left(), delta, allow_discrete, color_resolution_context);

        if (!interpolated_top || !interpolated_right || !interpolated_bottom || !interpolated_left)
            return {};

        return RectStyleValue::create(interpolated_top.release_nonnull(), interpolated_right.release_nonnull(), interpolated_bottom.release_nonnull(), interpolated_left.release_nonnull());
    }
    case StyleValue::Type::TextIndent: {
        auto& from_text_indent = from.as_text_indent();
        auto& to_text_indent = to.as_text_indent();

        if (from_text_indent.each_line() != to_text_indent.each_line()
            || from_text_indent.hanging() != to_text_indent.hanging())
            return {};

        auto interpolated_length_percentage = interpolate_value(element, calculation_context, from_text_indent.length_percentage(), to_text_indent.length_percentage(), delta, allow_discrete);
        if (!interpolated_length_percentage)
            return {};

        return TextIndentStyleValue::create(interpolated_length_percentage.release_nonnull(),
            from_text_indent.hanging() ? TextIndentStyleValue::Hanging::Yes : TextIndentStyleValue::Hanging::No,
            from_text_indent.each_line() ? TextIndentStyleValue::EachLine::Yes : TextIndentStyleValue::EachLine::No);
    }
    case StyleValue::Type::Transformation:
        VERIFY_NOT_REACHED();
    case StyleValue::Type::ValueList: {
        auto const& from_list = from.as_value_list();
        auto const& to_list = to.as_value_list();
        if (from_list.size() != to_list.size())
            return {};

        // FIXME: If the number of components or the types of corresponding components do not match,
        // or if any component value uses discrete animation and the two corresponding values do not match,
        // then the property values combine as discrete.
        StyleValueVector interpolated_values;
        interpolated_values.ensure_capacity(from_list.size());
        for (size_t i = 0; i < from_list.size(); ++i) {
            auto interpolated = interpolate_value(element, calculation_context, from_list.values()[i], to_list.values()[i], delta, AllowDiscrete::No, color_resolution_context);
            if (!interpolated)
                return {};

            interpolated_values.append(*interpolated);
        }

        return StyleValueList::create(move(interpolated_values), from_list.separator());
    }
    default:
        return {};
    }
}

RefPtr<StyleValue const> interpolate_repeatable_list(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete, ColorResolutionContext const* color_resolution_context)
{
    // https://www.w3.org/TR/web-animations/#repeatable-list
    // Same as by computed value except that if the two lists have differing numbers of items, they are first repeated to the least common multiple number of items.
    // Each item is then combined by computed value.
    // If a pair of values cannot be combined or if any component value uses discrete animation, then the property values combine as discrete.

    auto make_repeatable_list = [&](auto const& from_list, auto const& to_list, Function<void(NonnullRefPtr<StyleValue const>)> append_callback) -> bool {
        // If the number of components or the types of corresponding components do not match,
        // or if any component value uses discrete animation and the two corresponding values do not match,
        // then the property values combine as discrete
        auto list_size = AK::lcm(from_list.size(), to_list.size());
        for (size_t i = 0; i < list_size; ++i) {
            auto value = interpolate_value(element, calculation_context, from_list.value_at(i, true), to_list.value_at(i, true), delta, AllowDiscrete::No, color_resolution_context);
            if (!value)
                return false;
            append_callback(*value);
        }

        return true;
    };

    auto make_single_value_list = [&](auto const& value, size_t size, auto separator) {
        StyleValueVector values;
        values.ensure_capacity(size);
        for (size_t i = 0; i < size; ++i)
            values.append(value);
        return StyleValueList::create(move(values), separator);
    };

    NonnullRefPtr from_list = from;
    NonnullRefPtr to_list = to;
    if (!from.is_value_list() && to.is_value_list())
        from_list = make_single_value_list(from, to.as_value_list().size(), to.as_value_list().separator());
    else if (!to.is_value_list() && from.is_value_list())
        to_list = make_single_value_list(to, from.as_value_list().size(), from.as_value_list().separator());
    else if (!from.is_value_list() && !to.is_value_list())
        return interpolate_value(element, calculation_context, from, to, delta, allow_discrete, color_resolution_context);

    StyleValueVector interpolated_values;
    if (!make_repeatable_list(from_list->as_value_list(), to_list->as_value_list(), [&](auto const& value) { interpolated_values.append(value); }))
        return interpolate_discrete(from, to, delta, allow_discrete);
    return StyleValueList::create(move(interpolated_values), from_list->as_value_list().separator());
}

// https://drafts.csswg.org/filter-effects/#accumulation
static StyleValueVector accumulate_filter_function(StyleValueList const& underlying_list, StyleValueList const& animated_list, ColorResolutionContext const& color_resolution_context)
{
    // Accumulation of <filter-value-list>s follows the same matching and extending rules as interpolation, falling
    // back to replace behavior if the lists do not match. However instead of interpolating the matching
    // <filter-function> pairs, their arguments are arithmetically added together - except in the case of
    // <filter-function>s whose initial value for interpolation is 1, which combine using one-based addition:
    // Vresult = Va + Vb - 1

    if (contains_url(underlying_list) || contains_url(animated_list))
        return {};

    auto accumulate_filter = [&](FilterStyleValue const& underlying, FilterStyleValue const& animated) -> RefPtr<FilterStyleValue const> {
        if (underlying.kind() != animated.kind())
            return {};

        switch (underlying.kind()) {
        case FilterStyleValue::Kind::Blur: {
            auto const& underlying_blur = static_cast<BlurFilterStyleValue const&>(underlying);
            auto const& animated_blur = static_cast<BlurFilterStyleValue const&>(animated);
            return BlurFilterStyleValue::create(LengthStyleValue::create(Length::make_px(underlying_blur.resolved_radius() + animated_blur.resolved_radius())));
        }
        case FilterStyleValue::Kind::HueRotate: {
            auto const& underlying_rotate = static_cast<HueRotateFilterStyleValue const&>(underlying);
            auto const& animated_rotate = static_cast<HueRotateFilterStyleValue const&>(animated);
            return HueRotateFilterStyleValue::create(AngleStyleValue::create(Angle::make_degrees(underlying_rotate.angle_degrees() + animated_rotate.angle_degrees())));
        }
        case FilterStyleValue::Kind::Color: {
            auto const& underlying_color = static_cast<ColorFilterStyleValue const&>(underlying);
            auto const& animated_color = static_cast<ColorFilterStyleValue const&>(animated);
            if (underlying_color.operation() != animated_color.operation())
                return {};

            auto underlying_amount = underlying_color.resolved_amount();
            auto animated_amount = animated_color.resolved_amount();

            double accumulated;
            switch (underlying_color.operation()) {
            case Gfx::ColorFilterType::Brightness:
            case Gfx::ColorFilterType::Contrast:
            case Gfx::ColorFilterType::Opacity:
            case Gfx::ColorFilterType::Saturate:
                accumulated = underlying_amount + animated_amount - 1.0;
                break;
            case Gfx::ColorFilterType::Grayscale:
            case Gfx::ColorFilterType::Invert:
            case Gfx::ColorFilterType::Sepia:
                accumulated = underlying_amount + animated_amount;
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            return ColorFilterStyleValue::create(underlying_color.operation(), NumberStyleValue::create(accumulated));
        }
        case FilterStyleValue::Kind::DropShadow: {
            auto const& underlying_shadow = static_cast<DropShadowFilterStyleValue const&>(underlying);
            auto const& animated_shadow = static_cast<DropShadowFilterStyleValue const&>(animated);

            auto add_lengths = [](NonnullRefPtr<StyleValue const> const& a, NonnullRefPtr<StyleValue const> const& b) -> NonnullRefPtr<StyleValue const> {
                auto a_value = Length::from_style_value(a, {}).absolute_length_to_px_without_rounding();
                auto b_value = Length::from_style_value(b, {}).absolute_length_to_px_without_rounding();

                return LengthStyleValue::create(Length::make_px(a_value + b_value));
            };

            auto offset_x = add_lengths(underlying_shadow.offset_x(), animated_shadow.offset_x());
            auto offset_y = add_lengths(underlying_shadow.offset_y(), animated_shadow.offset_y());
            RefPtr<StyleValue const> accumulated_radius;
            if (underlying_shadow.radius() || animated_shadow.radius()) {
                auto underlying_radius = underlying_shadow.radius() ? Length::from_style_value(*underlying_shadow.radius(), {}).absolute_length_to_px_without_rounding() : 0;
                auto animated_radius = animated_shadow.radius() ? Length::from_style_value(*animated_shadow.radius(), {}).absolute_length_to_px_without_rounding() : 0;
                accumulated_radius = LengthStyleValue::create(Length::make_px(underlying_radius + animated_radius));
            }

            auto element_color = color_resolution_context.current_color.value_or(Color::Black);
            auto resolve_color = [&](RefPtr<StyleValue const> const& color) {
                return color ? color->to_color(color_resolution_context).value_or(element_color) : element_color;
            };
            auto underlying_color = resolve_color(underlying_shadow.color());
            auto animated_color = resolve_color(animated_shadow.color());
            auto accumulated = Color(
                min(255, underlying_color.red() + animated_color.red()),
                min(255, underlying_color.green() + animated_color.green()),
                min(255, underlying_color.blue() + animated_color.blue()),
                min(255, underlying_color.alpha() + animated_color.alpha()));
            auto accumulated_color = ColorStyleValue::create_from_color(accumulated, ColorSyntax::Legacy);

            return DropShadowFilterStyleValue::create(
                offset_x,
                offset_y,
                accumulated_radius,
                accumulated_color);
        }
        }
        VERIFY_NOT_REACHED();
    };

    // Extend shorter list with initial values
    size_t max_size = max(underlying_list.size(), animated_list.size());
    StyleValueVector extended_underlying;
    StyleValueVector extended_animated;

    for (size_t i = 0; i < max_size; ++i) {
        if (i < underlying_list.size())
            extended_underlying.append(underlying_list.values()[i]);
        else
            extended_underlying.append(FilterStyleValue::initial_value_for(animated_list.values()[i]->as_filter(), false));

        if (i < animated_list.size())
            extended_animated.append(animated_list.values()[i]);
        else
            extended_animated.append(FilterStyleValue::initial_value_for(underlying_list.values()[i]->as_filter(), false));
    }

    StyleValueVector result;
    result.ensure_capacity(max_size);
    for (size_t i = 0; i < max_size; ++i) {
        auto accumulated = accumulate_filter(extended_underlying[i]->as_filter(), extended_animated[i]->as_filter());
        if (!accumulated)
            return {};
        result.unchecked_append(accumulated.release_nonnull());
    }
    return result;
}

RefPtr<StyleValue const> interpolate_value(DOM::Element& element, CalculationContext const& calculation_context, StyleValue const& from, StyleValue const& to, float delta, AllowDiscrete allow_discrete, ColorResolutionContext const* color_resolution_context)
{
    if (auto result = interpolate_value_impl(element, calculation_context, from, to, delta, allow_discrete, color_resolution_context))
        return result;
    return interpolate_discrete(from, to, delta, allow_discrete);
}

static Optional<GridTrackSizeList> composite_grid_track_size_list(PropertyID property_id, CalculationContext const& calculation_context, GridTrackSizeList const& underlying, GridTrackSizeList const& animated, Bindings::CompositeOperation composite_operation)
{
    // https://drafts.csswg.org/css-grid-2/#track-sizing
    // Animation type: if the list lengths match, by computed value type per item in the computed track list;
    // discrete otherwise.
    //
    // https://drafts.csswg.org/css-grid-2/#computed-track-list-subgrid
    // The computed track list of a subgrid axis is the subgrid keyword followed by a list of line names.
    if (underlying.is_subgrid() || animated.is_subgrid())
        return {};

    auto composite_grid_size = [&](GridSize const& underlying_grid_size, GridSize const& animated_grid_size) -> Optional<GridSize> {
        if (auto composited_value = composite_value(property_id, underlying_grid_size.style_value(), animated_grid_size.style_value(), composite_operation))
            return GridSize { *composited_value };

        return {};
    };

    auto expanded_underlying = expand_grid_tracks_and_lines(underlying);
    auto expanded_animated = expand_grid_tracks_and_lines(animated);

    if (expanded_underlying.tracks.size() != expanded_animated.tracks.size())
        return {};

    GridTrackSizeList result;
    for (size_t i = 0; i < expanded_underlying.tracks.size(); ++i) {
        auto& underlying_track = expanded_underlying.tracks[i];
        auto& animated_track = expanded_animated.tracks[i];
        auto composited_line_names = move(expanded_animated.line_names[i]);

        if (underlying_track.is_repeat() || animated_track.is_repeat()) {
            if (!underlying_track.is_repeat() || !animated_track.is_repeat())
                return {};

            auto underlying_repeat = underlying_track.repeat();
            auto animated_repeat = animated_track.repeat();
            if (!underlying_repeat.is_fixed() || !animated_repeat.is_fixed())
                return {};
            if (underlying_repeat.repeat_count() != animated_repeat.repeat_count() || underlying_repeat.grid_track_size_list().track_list().size() != animated_repeat.grid_track_size_list().track_list().size())
                return {};

            auto composited_repeat_grid_tracks = composite_grid_track_size_list(property_id, calculation_context, underlying_repeat.grid_track_size_list(), animated_repeat.grid_track_size_list(), composite_operation);
            if (!composited_repeat_grid_tracks.has_value())
                return {};

            ExplicitGridTrack composited_grid_track { GridRepeat { underlying_repeat.type(), move(*composited_repeat_grid_tracks), IntegerStyleValue::create(underlying_repeat.repeat_count()) } };
            append_grid_track_with_line_names(result, move(composited_grid_track), move(composited_line_names));
            continue;
        }

        if (underlying_track.is_minmax() && animated_track.is_minmax()) {
            auto underlying_minmax = underlying_track.minmax();
            auto animated_minmax = animated_track.minmax();
            auto composited_min = composite_grid_size(underlying_minmax.min_grid_size(), animated_minmax.min_grid_size());
            auto composited_max = composite_grid_size(underlying_minmax.max_grid_size(), animated_minmax.max_grid_size());
            ExplicitGridTrack composited_grid_track { GridMinMax {
                composited_min.value_or(animated_minmax.min_grid_size()),
                composited_max.value_or(animated_minmax.max_grid_size()) } };
            append_grid_track_with_line_names(result, move(composited_grid_track), move(composited_line_names));
            continue;
        }
        if (underlying_track.is_default() && animated_track.is_default()) {
            auto const& underlying_grid_size = underlying_track.grid_size();
            auto const& animated_grid_size = animated_track.grid_size();
            auto composited_grid_size_result = composite_grid_size(underlying_grid_size, animated_grid_size);
            if (composited_grid_size_result.has_value()) {
                ExplicitGridTrack composited_grid_track { move(*composited_grid_size_result) };
                append_grid_track_with_line_names(result, move(composited_grid_track), move(composited_line_names));
                continue;
            }
        }
        append_grid_track_with_line_names(result, animated_track, move(composited_line_names));
    }
    return result;
}

static RefPtr<StyleValue const> composite_mixed_value(StyleValue const& underlying_value, StyleValue const& animated_value, CalculationContext const& calculation_context)
{
    // https://drafts.csswg.org/css-values-4/#combine-mixed
    // Addition of <percentage> is defined the same as interpolation except by adding each component rather than interpolating it.
    auto underlying_value_type = get_value_type_of_numeric_style_value(underlying_value, calculation_context);
    auto animated_value_type = get_value_type_of_numeric_style_value(animated_value, calculation_context);

    if (underlying_value_type.has_value() && underlying_value_type == animated_value_type) {
        // The computed value of a percentage-dimension mix is defined as
        // FIXME: a computed dimension if the percentage component is zero or is defined specifically to compute to a dimension value
        // a computed percentage if the dimension component is zero
        // a computed calc() expression otherwise
        if (auto const* from_dimension_value = as_if<DimensionStyleValue>(underlying_value); from_dimension_value && animated_value.type() == StyleValue::Type::Percentage) {
            auto dimension_component = from_dimension_value->raw_value();
            auto percentage_component = animated_value.as_percentage().raw_value();
            if (dimension_component == 0.f)
                return PercentageStyleValue::create(Percentage { percentage_component });
        } else if (auto const* to_dimension_value = as_if<DimensionStyleValue>(animated_value); to_dimension_value && underlying_value.type() == StyleValue::Type::Percentage) {
            auto dimension_component = to_dimension_value->raw_value();
            auto percentage_component = underlying_value.as_percentage().raw_value();
            if (dimension_component == 0)
                return PercentageStyleValue::create(Percentage { percentage_component });
        }

        Vector<CalcNodeRef> contributions;
        contributions.append(CalcNodeRef::from_style_value(underlying_value));
        contributions.append(CalcNodeRef::from_style_value(animated_value));
        auto composited_sum = CalcNodeRef::sum(move(contributions));

        auto numeric_type = composited_sum.determine_type(calculation_context);
        return CalculatedStyleValue::create(
            simplify_a_calculation_tree(composited_sum, calculation_context, {}),
            numeric_type.value(),
            calculation_context);
    }

    return {};
}

RefPtr<StyleValue const> composite_value(PropertyID property_id, StyleValue const& underlying_value, StyleValue const& animated_value, Bindings::CompositeOperation composite_operation, ColorResolutionContext const& color_resolution_context)
{
    auto ffi_composite_operation = [&] {
        switch (composite_operation) {
        case Bindings::CompositeOperation::Replace:
            return StyleValueFFI::FfiCompositeOperation::Replace;
        case Bindings::CompositeOperation::Add:
            return StyleValueFFI::FfiCompositeOperation::Add;
        case Bindings::CompositeOperation::Accumulate:
            return StyleValueFFI::FfiCompositeOperation::Accumulate;
        }
        VERIFY_NOT_REACHED();
    }();
    auto rust_result = StyleValueFFI::rust_composite_scalar_style_value(
        underlying_value.rust_style_value_data(),
        animated_value.rust_style_value_data(),
        ffi_composite_operation);
    if (rust_result.handled) {
        if (!rust_result.value)
            return {};
        return StyleValue::adopt_rust_style_value_data(rust_result.value);
    }

    auto calculation_context = CalculationContext::for_property(PropertyNameAndID::from_id(property_id));

    if (composite_operation == Bindings::CompositeOperation::Replace)
        return {};

    if (underlying_value.type() != animated_value.type() || underlying_value.is_calculated() || animated_value.is_calculated())
        return composite_mixed_value(underlying_value, animated_value, calculation_context);

    switch (underlying_value.type()) {
    case StyleValue::Type::BasicShape: {
        auto const& underlying_basic_shape = underlying_value.as_basic_shape();
        auto const& animated_basic_shape = animated_value.as_basic_shape();

        if (underlying_basic_shape.basic_shape().index() != animated_basic_shape.basic_shape().index())
            return {};

        return underlying_basic_shape.basic_shape().visit(
            [&](Inset const& underlying_inset) -> RefPtr<StyleValue const> {
                auto const& animated_inset = animated_basic_shape.basic_shape().get<Inset>();
                auto composited_top = composite_value(property_id, underlying_inset.top, animated_inset.top, composite_operation);
                auto composited_right = composite_value(property_id, underlying_inset.right, animated_inset.right, composite_operation);
                auto composited_bottom = composite_value(property_id, underlying_inset.bottom, animated_inset.bottom, composite_operation);
                auto composited_left = composite_value(property_id, underlying_inset.left, animated_inset.left, composite_operation);
                auto composited_border_radius = composite_value(property_id, underlying_inset.border_radius, animated_inset.border_radius, composite_operation);
                if (!composited_top || !composited_right || !composited_bottom || !composited_left || !composited_border_radius)
                    return {};

                return BasicShapeStyleValue::create(Inset { composited_top.release_nonnull(), composited_right.release_nonnull(), composited_bottom.release_nonnull(), composited_left.release_nonnull(), composited_border_radius.release_nonnull() });
            },
            [&](Circle const& underlying_circle) -> RefPtr<StyleValue const> {
                auto const& animated_circle = animated_basic_shape.basic_shape().get<Circle>();
                auto composited_radius = composite_value(property_id, underlying_circle.radius, animated_circle.radius, composite_operation);
                if (!composited_radius)
                    return {};

                RefPtr<StyleValue const> composited_position;
                if (underlying_circle.position || animated_circle.position) {
                    auto const& underlying_position_with_default = underlying_circle.position ? ValueComparingNonnullRefPtr<StyleValue const> { *underlying_circle.position } : PositionStyleValue::create_computed_center();
                    auto const& animated_position_with_default = animated_circle.position ? ValueComparingNonnullRefPtr<StyleValue const> { *animated_circle.position } : PositionStyleValue::create_computed_center();

                    composited_position = composite_value(property_id, underlying_position_with_default, animated_position_with_default, composite_operation);

                    if (!composited_position)
                        return {};
                }

                return BasicShapeStyleValue::create(Circle { composited_radius.release_nonnull(), composited_position });
            },
            [&](Ellipse const& underlying_ellipse) -> RefPtr<StyleValue const> {
                auto const& animated_ellipse = animated_basic_shape.basic_shape().get<Ellipse>();
                auto composited_radius = composite_value(property_id, underlying_ellipse.radius, animated_ellipse.radius, composite_operation);
                if (!composited_radius)
                    return {};

                RefPtr<StyleValue const> composited_position;
                if (underlying_ellipse.position || animated_ellipse.position) {
                    auto const& underlying_position_with_default = underlying_ellipse.position ? ValueComparingNonnullRefPtr<StyleValue const> { *underlying_ellipse.position } : PositionStyleValue::create_computed_center();
                    auto const& animated_position_with_default = animated_ellipse.position ? ValueComparingNonnullRefPtr<StyleValue const> { *animated_ellipse.position } : PositionStyleValue::create_computed_center();

                    composited_position = composite_value(property_id, underlying_position_with_default, animated_position_with_default, composite_operation);

                    if (!composited_position)
                        return {};
                }

                return BasicShapeStyleValue::create(Ellipse { composited_radius.release_nonnull(), composited_position });
            },
            [&](Polygon const& underlying_polygon) -> RefPtr<StyleValue const> {
                auto const& animated_polygon = animated_basic_shape.basic_shape().get<Polygon>();
                if (underlying_polygon.fill_rule != animated_polygon.fill_rule)
                    return {};

                if (underlying_polygon.points.size() != animated_polygon.points.size())
                    return {};

                Vector<Polygon::Point> composited_points;
                composited_points.ensure_capacity(underlying_polygon.points.size());
                for (size_t i = 0; i < underlying_polygon.points.size(); i++) {
                    auto const& underlying_point = underlying_polygon.points[i];
                    auto const& animated_point = animated_polygon.points[i];
                    auto composited_point_x = composite_value(property_id, underlying_point.x, animated_point.x, composite_operation);
                    auto composited_point_y = composite_value(property_id, underlying_point.y, animated_point.y, composite_operation);
                    if (!composited_point_x || !composited_point_y)
                        return {};
                    composited_points.unchecked_append(Polygon::Point { *composited_point_x, *composited_point_y });
                }

                return BasicShapeStyleValue::create(Polygon { underlying_polygon.fill_rule, move(composited_points) });
            },
            [&](Xywh const&) -> RefPtr<StyleValue const> {
                // xywh() should have been absolutized into inset() before now
                VERIFY_NOT_REACHED();
            },
            [&](Rect const&) -> RefPtr<StyleValue const> {
                // rect() should have been absolutized into inset() before now
                VERIFY_NOT_REACHED();
            },
            [&](Path const&) -> RefPtr<StyleValue const> {
                // FIXME: Implement composition for path()
                return {};
            });
    }
    case StyleValue::Type::BorderImageSlice: {
        auto& underlying_border_image_slice_value = underlying_value.as_border_image_slice();
        auto& animated_border_image_slice_value = animated_value.as_border_image_slice();
        if (underlying_border_image_slice_value.fill() != animated_border_image_slice_value.fill())
            return {};
        auto composited_top = composite_value(property_id, underlying_border_image_slice_value.top(), animated_border_image_slice_value.top(), composite_operation);
        auto composited_right = composite_value(property_id, underlying_border_image_slice_value.right(), animated_border_image_slice_value.right(), composite_operation);
        auto composited_bottom = composite_value(property_id, underlying_border_image_slice_value.bottom(), animated_border_image_slice_value.bottom(), composite_operation);
        auto composited_left = composite_value(property_id, underlying_border_image_slice_value.left(), animated_border_image_slice_value.left(), composite_operation);
        if (!composited_top || !composited_right || !composited_bottom || !composited_left)
            return {};
        return BorderImageSliceStyleValue::create(composited_top.release_nonnull(), composited_right.release_nonnull(), composited_bottom.release_nonnull(), composited_left.release_nonnull(), underlying_border_image_slice_value.fill());
    }
    case StyleValue::Type::BorderRadius: {
        auto composited_horizontal_radius = composite_value(property_id, underlying_value.as_border_radius().horizontal_radius(), animated_value.as_border_radius().horizontal_radius(), composite_operation);
        auto composited_vertical_radius = composite_value(property_id, underlying_value.as_border_radius().vertical_radius(), animated_value.as_border_radius().vertical_radius(), composite_operation);
        if (!composited_horizontal_radius || !composited_vertical_radius)
            return {};
        return BorderRadiusStyleValue::create(composited_horizontal_radius.release_nonnull(), composited_vertical_radius.release_nonnull());
    }
    case StyleValue::Type::BorderRadiusRect: {
        auto const& underlying_top_left = underlying_value.as_border_radius_rect().top_left();
        auto const& animated_top_left = animated_value.as_border_radius_rect().top_left();

        auto const& underlying_top_right = underlying_value.as_border_radius_rect().top_right();
        auto const& animated_top_right = animated_value.as_border_radius_rect().top_right();
        auto const& underlying_bottom_right = underlying_value.as_border_radius_rect().bottom_right();
        auto const& animated_bottom_right = animated_value.as_border_radius_rect().bottom_right();

        auto const& underlying_bottom_left = underlying_value.as_border_radius_rect().bottom_left();
        auto const& animated_bottom_left = animated_value.as_border_radius_rect().bottom_left();

        auto composited_top_left = composite_value(property_id, underlying_top_left, animated_top_left, composite_operation);
        auto composited_top_right = composite_value(property_id, underlying_top_right, animated_top_right, composite_operation);
        auto composited_bottom_right = composite_value(property_id, underlying_bottom_right, animated_bottom_right, composite_operation);
        auto composited_bottom_left = composite_value(property_id, underlying_bottom_left, animated_bottom_left, composite_operation);

        if (!composited_top_left || !composited_top_right || !composited_bottom_right || !composited_bottom_left)
            return {};

        return BorderRadiusRectStyleValue::create(composited_top_left.release_nonnull(), composited_top_right.release_nonnull(), composited_bottom_right.release_nonnull(), composited_bottom_left.release_nonnull());
    }
    case StyleValue::Type::Edge: {
        auto const& underlying_offset = underlying_value.as_edge().offset();
        auto const& animated_offset = animated_value.as_edge().offset();

        if (auto composited_value = composite_value(property_id, underlying_offset, animated_offset, composite_operation))
            return EdgeStyleValue::create({}, composited_value);

        return {};
    }
    case StyleValue::Type::Function: {
        auto const& underlying_function = underlying_value.as_function();
        auto const& animated_function = animated_value.as_function();

        if (underlying_function.name() != animated_function.name())
            return {};

        auto composited_value = composite_value(property_id, underlying_function.value(), animated_function.value(), composite_operation);
        if (!composited_value)
            return {};

        return FunctionStyleValue::create(underlying_function.name(), composited_value.release_nonnull());
    }
    case StyleValue::Type::GridTrackSizeList: {
        auto underlying_list = underlying_value.as_grid_track_size_list().grid_track_size_list();
        auto animated_list = animated_value.as_grid_track_size_list().grid_track_size_list();
        auto composited_list = composite_grid_track_size_list(property_id, calculation_context, underlying_list, animated_list, composite_operation);
        if (!composited_list.has_value())
            return {};
        return GridTrackSizeListStyleValue::create(composited_list.release_value());
    }
    case StyleValue::Type::OpenTypeTagged: {
        auto& underlying_open_type_tagged = underlying_value.as_open_type_tagged();
        auto& animated_open_type_tagged = animated_value.as_open_type_tagged();
        if (underlying_open_type_tagged.tag() != animated_open_type_tagged.tag())
            return {};
        auto composited_value = composite_value(property_id, underlying_open_type_tagged.value(), animated_open_type_tagged.value(), composite_operation);
        if (!composited_value)
            return {};
        return OpenTypeTaggedStyleValue::create(OpenTypeTaggedStyleValue::Mode::FontVariationSettings, underlying_open_type_tagged.tag(), composited_value.release_nonnull());
    }
    case StyleValue::Type::Position: {
        auto& underlying_position = underlying_value.as_position();
        auto& animated_position = animated_value.as_position();
        auto composited_edge_x = composite_value(property_id, underlying_position.edge_x(), animated_position.edge_x(), composite_operation);
        auto composited_edge_y = composite_value(property_id, underlying_position.edge_y(), animated_position.edge_y(), composite_operation);
        if (!composited_edge_x || !composited_edge_y)
            return {};

        return PositionStyleValue::create(composited_edge_x->as_edge(), composited_edge_y->as_edge());
    }
    case StyleValue::Type::RadialSize: {
        auto const& underlying_components = underlying_value.as_radial_size().components();
        auto const& animated_components = animated_value.as_radial_size().components();

        auto const is_radial_extent = [](auto const& component) { return component.template has<RadialExtent>(); };

        // https://drafts.csswg.org/css-images-4/#interpolating-gradients
        // https://drafts.csswg.org/css-shapes-1/#basic-shape-interpolation
        // FIXME: Radial extents should disallow composition for basic-shape values but should be converted into their
        //        equivalent length-percentage values for radial gradients
        if (any_of(underlying_components, is_radial_extent) || any_of(animated_components, is_radial_extent))
            return {};

        if (underlying_components.size() == 1 && animated_components.size() == 1) {
            auto const& underlying_component = underlying_components[0].get<NonnullRefPtr<StyleValue const>>();
            auto const& animated_component = animated_components[0].get<NonnullRefPtr<StyleValue const>>();

            auto interpolated_value = composite_value(property_id, underlying_component, animated_component, composite_operation);
            if (!interpolated_value)
                return {};

            return RadialSizeStyleValue::create({ interpolated_value.release_nonnull() });
        }

        auto const& underlying_horizontal_component = underlying_components[0].get<NonnullRefPtr<StyleValue const>>();
        auto const& underlying_vertical_component = underlying_components.size() > 1 ? underlying_components[1].get<NonnullRefPtr<StyleValue const>>() : underlying_horizontal_component;

        auto const& animated_horizontal_component = animated_components[0].get<NonnullRefPtr<StyleValue const>>();
        auto const& animated_vertical_component = animated_components.size() > 1 ? animated_components[1].get<NonnullRefPtr<StyleValue const>>() : animated_horizontal_component;
        auto composited_horizontal = composite_value(property_id, underlying_horizontal_component, animated_horizontal_component, composite_operation);
        auto composited_vertical = composite_value(property_id, underlying_vertical_component, animated_vertical_component, composite_operation);

        if (!composited_horizontal || !composited_vertical)
            return {};

        return RadialSizeStyleValue::create({ composited_horizontal.release_nonnull(), composited_vertical.release_nonnull() });
    }
    case StyleValue::Type::ValueList: {
        auto& underlying_list = underlying_value.as_value_list();
        auto& animated_list = animated_value.as_value_list();

        if (is_filter_style_value_list(underlying_value) && is_filter_style_value_list(animated_value)) {
            // https://drafts.csswg.org/filter-effects/#addition
            // Given two filter values representing an base value (base filter list) and a value to add (added filter list),
            // returns the concatenation of the the two lists: ‘base filter list added filter list’.
            if (composite_operation == Bindings::CompositeOperation::Add) {
                StyleValueVector result { underlying_list.values() };
                result.extend(StyleValueVector { animated_list.values() });
                return StyleValueList::create(move(result), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
            }

            VERIFY(composite_operation == Bindings::CompositeOperation::Accumulate);
            auto result = accumulate_filter_function(underlying_list, animated_list, color_resolution_context);
            if (result.is_empty())
                return {};

            return StyleValueList::create(move(result), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
        }

        if (underlying_list.size() != animated_list.size() || underlying_list.separator() != animated_list.separator())
            return {};
        StyleValueVector values;
        values.ensure_capacity(underlying_list.size());
        for (size_t i = 0; i < underlying_list.size(); ++i) {
            auto composited_value = composite_value(property_id, underlying_list.values()[i], animated_list.values()[i], composite_operation);
            if (!composited_value)
                return {};
            values.unchecked_append(*composited_value);
        }
        return StyleValueList::create(move(values), underlying_list.separator());
    }
    default:
        // FIXME: Implement compositing for missing types
        return {};
    }
}

}
