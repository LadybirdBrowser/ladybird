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

}
