/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/RustStyleBridge.h>
#include <LibWeb/CSS/StyleGroupPayloadPins.h>
#include <LibWeb/ComputedValuesRustFFI.h>

namespace Web::CSS {

StyleGroupPayloadPins::~StyleGroupPayloadPins()
{
    clear();
}

void StyleGroupPayloadPins::set(ReadonlySpan<void const*> payloads)
{
    clear();
    if (payloads.is_empty())
        return;
    m_payloads.ensure_capacity(payloads.size());
    ComputedValuesFFI::rust_style_groups_retain(payloads.data(), payloads.size());
    for (auto const* payload : payloads)
        m_payloads.unchecked_append(payload);
}

void StyleGroupPayloadPins::clear()
{
    if (!m_payloads.is_empty())
        ComputedValuesFFI::rust_style_groups_release(m_payloads.data(), m_payloads.size());
    m_payloads.clear_with_capacity();
}

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

ComputedValuesFFI::FfiPropertyNumericRange const* invoke_rust_property_metadata_numeric_ranges(u16 property_id, size_t* length)
{
    return ComputedValuesFFI::rust_property_metadata_numeric_ranges(property_id, length);
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

StyleValueFFI::FfiNumericType invoke_rust_numeric_type_operate(StyleValueFFI::FfiNumericTypeOperation operation, StyleValueFFI::FfiNumericType const* first, StyleValueFFI::FfiNumericType const* second)
{
    return StyleValueFFI::rust_numeric_type_operate(operation, first, second);
}

bool invoke_rust_numeric_type_matches(StyleValueFFI::FfiNumericTypeMatch match_kind, StyleValueFFI::FfiNumericType const* numeric_type, u8 base_type, bool has_percentages_resolve_as, u8 percentages_resolve_as)
{
    return StyleValueFFI::rust_numeric_type_matches(match_kind, numeric_type, base_type, has_percentages_resolve_as, percentages_resolve_as);
}

}
