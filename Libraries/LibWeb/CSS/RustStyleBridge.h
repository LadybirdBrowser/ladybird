/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

WEB_API void* clone_rust_style_group(size_t group_index, void const* payload);
WEB_API void free_rust_style_group(size_t group_index, void* payload);

WEB_API void invoke_rust_property_metadata_bounds(u16* first_longhand, u16* last_longhand, u16* first_inherited, u16* last_inherited);
WEB_API bool invoke_rust_property_metadata_is_inherited(u16 property_id);
WEB_API u16 const* invoke_rust_property_metadata_computation_order(size_t* length);
WEB_API u16 invoke_rust_map_logical_alias_to_physical(u16 property_id, u8 writing_mode, u8 direction);
WEB_API u16 invoke_rust_map_physical_to_logical_alias(u16 property_id, u8 writing_mode, u8 direction);
WEB_API bool invoke_rust_property_metadata_is_shorthand(u16 property_id);
WEB_API u16 const* invoke_rust_property_metadata_longhands_for_shorthand(u16 property_id, size_t* length);
WEB_API u8 invoke_rust_property_metadata_requires_computation_level(u16 property_id);

WEB_API ComputedValuesFFI::FfiAbsolutizedLength invoke_rust_absolutize_length(double value, u8 unit, ComputedValuesFFI::FfiLengthResolutionContext const* context);
WEB_API i32 rust_css_pixels_multiply(i32 left, i32 right);
WEB_API i32 rust_css_pixels_divide_as_fraction(i32 numerator, i32 denominator);
WEB_API i32 rust_css_pixels_nearest_value_for(double value);
WEB_API i32 rust_css_pixels_scaled(i32 value, double factor);

WEB_API StyleValueFFI::FfiNumericType invoke_rust_numeric_type_operate(u8 operation, StyleValueFFI::FfiNumericType const* first, StyleValueFFI::FfiNumericType const* second);

}
