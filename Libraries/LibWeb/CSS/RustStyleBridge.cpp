/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

void* clone_rust_style_group(size_t group_index, void const* payload)
{
    return ComputedValuesFFI::rust_style_group_clone(group_index, payload);
}

void free_rust_style_group(size_t group_index, void* payload)
{
    ComputedValuesFFI::rust_style_group_free(group_index, payload);
}

void invoke_rust_property_metadata_bounds(u16* first_longhand, u16* last_longhand, u16* first_inherited, u16* last_inherited)
{
    ComputedValuesFFI::rust_property_metadata_bounds(first_longhand, last_longhand, first_inherited, last_inherited);
}

bool invoke_rust_property_metadata_is_inherited(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_is_inherited(property_id);
}

u16 const* invoke_rust_property_metadata_computation_order(size_t* length)
{
    return ComputedValuesFFI::rust_property_metadata_computation_order(length);
}

u16 invoke_rust_map_logical_alias_to_physical(u16 property_id, u8 writing_mode, u8 direction)
{
    return ComputedValuesFFI::rust_map_logical_alias_to_physical(property_id, writing_mode, direction);
}

u16 invoke_rust_map_physical_to_logical_alias(u16 property_id, u8 writing_mode, u8 direction)
{
    return ComputedValuesFFI::rust_map_physical_to_logical_alias(property_id, writing_mode, direction);
}

bool invoke_rust_property_metadata_is_shorthand(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_is_shorthand(property_id);
}

u16 const* invoke_rust_property_metadata_longhands_for_shorthand(u16 property_id, size_t* length)
{
    return ComputedValuesFFI::rust_property_metadata_longhands_for_shorthand(property_id, length);
}

u8 invoke_rust_property_metadata_requires_computation_level(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_requires_computation_level(property_id);
}

u8 invoke_rust_property_metadata_animation_type(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_animation_type(property_id);
}

ComputedValuesFFI::FfiPropertyNumericRange const* invoke_rust_property_metadata_numeric_ranges(u16 property_id, size_t* length)
{
    return ComputedValuesFFI::rust_property_metadata_numeric_ranges(property_id, length);
}

bool invoke_rust_animation_property_is_preferred(u16 a, u16 b)
{
    return ComputedValuesFFI::rust_animation_property_is_preferred(a, b);
}

bool invoke_cpp_animation_property_is_preferred(u16 a, u16 b)
{
    auto property_is_logical_alias_including_shorthands = [](PropertyID property_id) {
        if (property_is_shorthand(property_id))
            return property_is_logical_alias(expanded_longhands_for_shorthand(property_id)[0]);
        return property_is_logical_alias(property_id);
    };
    auto property_a = static_cast<PropertyID>(a);
    auto property_b = static_cast<PropertyID>(b);
    if (property_is_shorthand(property_a) != property_is_shorthand(property_b))
        return !property_is_shorthand(property_a);
    if (property_is_shorthand(property_a)) {
        auto a_length = expanded_longhands_for_shorthand(property_a).size();
        auto b_length = expanded_longhands_for_shorthand(property_b).size();
        if (a_length != b_length)
            return a_length < b_length;
    }
    auto a_is_logical_alias = property_is_logical_alias_including_shorthands(property_a);
    auto b_is_logical_alias = property_is_logical_alias_including_shorthands(property_b);
    if (a_is_logical_alias != b_is_logical_alias)
        return !a_is_logical_alias;
    return camel_case_string_from_property_id(property_a) < camel_case_string_from_property_id(property_b);
}

StyleValueFFI::StyleValueData const* invoke_rust_style_metadata_initial_value(u16 property_id)
{
    return static_cast<StyleValueFFI::StyleValueData const*>(ComputedValuesFFI::rust_style_metadata_initial_value(property_id));
}

ComputedValuesFFI::FfiAbsolutizedLength invoke_rust_absolutize_length(double value, u8 unit, ComputedValuesFFI::FfiLengthResolutionContext const* context)
{
    return ComputedValuesFFI::rust_absolutize_length(value, unit, context);
}

i32 rust_css_pixels_multiply(i32 left, i32 right)
{
    return ComputedValuesFFI::rust_css_pixels_multiply(left, right);
}

i32 rust_css_pixels_divide_as_fraction(i32 numerator, i32 denominator)
{
    return ComputedValuesFFI::rust_css_pixels_divide_as_fraction(numerator, denominator);
}

i32 rust_css_pixels_nearest_value_for(double value)
{
    return ComputedValuesFFI::rust_css_pixels_nearest_value_for(value);
}

i32 rust_css_pixels_scaled(i32 value, double factor)
{
    return ComputedValuesFFI::rust_css_pixels_scaled(value, factor);
}

StyleValueFFI::FfiNumericType invoke_rust_numeric_type_operate(u8 operation, StyleValueFFI::FfiNumericType const* first, StyleValueFFI::FfiNumericType const* second)
{
    return StyleValueFFI::rust_numeric_type_operate(operation, first, second);
}

}
