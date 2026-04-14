/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibGfx/ColorConversion.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

struct MissingComponents {
    Array<bool, 3> components { false, false, false };
    bool alpha { false };

    constexpr MissingComponents() = default;
    constexpr MissingComponents(bool first, bool second, bool third, bool alpha_value = false)
        : components { first, second, third }
        , alpha(alpha_value)
    {
    }

    bool& component(size_t index) { return components[index]; }
    bool component(size_t index) const { return components[index]; }
};

enum class ComponentCategory : u8 {
    Red,
    Green,
    Blue,
    Lightness,
    Colorfulness,
    Hue,
    OpponentA,
    OpponentB,
    NotAnalogous,
};

struct ComponentCategories {
    Array<ComponentCategory, 3> components { ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous, ComponentCategory::NotAnalogous };

    constexpr ComponentCategories() = default;
    constexpr ComponentCategories(ComponentCategory first, ComponentCategory second, ComponentCategory third)
        : components { first, second, third }
    {
    }

    ComponentCategory component(size_t index) const { return components[index]; }
    bool operator==(ComponentCategories const&) const = default;
};

struct InterpolationPolicy {
    bool use_legacy_output;
    ColorInterpolationMethodStyleValue::ColorInterpolationMethod color_interpolation_method;
};

struct InterpolationSpaceState {
    Gfx::ColorComponents from_components;
    Gfx::ColorComponents to_components;
    MissingComponents from_missing;
    MissingComponents to_missing;
    size_t hue_index { 0 };
    HueInterpolationMethod hue_interpolation_method { HueInterpolationMethod::Shorter };
    PolarColorSpace polar_color_space { PolarColorSpace::Hsl };
    RectangularColorSpace rectangular_color_space { RectangularColorSpace::Srgb };
    bool is_polar { false };
    ComponentCategories polar_target_categories {};
};

struct InterpolatedColor {
    Gfx::ColorComponents components;
    MissingComponents missing;
    InterpolationPolicy policy;
    InterpolationSpaceState state;
};

Optional<InterpolatedColor> perform_color_interpolation(
    StyleValue const& from, StyleValue const& to, float delta,
    Optional<ColorInterpolationMethodStyleValue::ColorInterpolationMethod> color_interpolation_method,
    ColorResolutionContext const& color_resolution_context);

RefPtr<StyleValue const> style_value_for_interpolated_color(InterpolatedColor const&);

}
