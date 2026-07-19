/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void rust_property_metadata_bounds(u16* first_longhand, u16* last_longhand, u16* first_inherited, u16* last_inherited)
{
    ComputedValuesFFI::rust_property_metadata_bounds(first_longhand, last_longhand, first_inherited, last_inherited);
}

bool rust_property_metadata_is_inherited(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_is_inherited(property_id);
}

u16 const* rust_property_metadata_computation_order(size_t* length)
{
    return ComputedValuesFFI::rust_property_metadata_computation_order(length);
}

u8 rust_property_metadata_requires_computation_level(u16 property_id)
{
    return ComputedValuesFFI::rust_property_metadata_requires_computation_level(property_id);
}

ComputedValuesFFI::FfiAbsolutizedLength rust_absolutize_length(double value, u8 unit, ComputedValuesFFI::FfiLengthResolutionContext const* context)
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

}
