/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/ComputedValuesRustFFI.h>

// Conversions between the C++ style computation context types and their
// mirrors in the Rust style computation core.

namespace Web::CSS {

inline constexpr u8 all_container_relative_length_units_mask = (1 << 6) - 1;

inline ComputedValuesFFI::FfiLengthResolutionContext to_ffi_length_resolution_context(Length::ResolutionContext const& context)
{
    return {
        .viewport_width = context.viewport_rect.width().to_double(),
        .viewport_height = context.viewport_rect.height().to_double(),
        .font_metrics = context.font_metrics,
        .root_font_metrics = context.root_font_metrics,
        .font_metrics_depend_on_viewport_metrics = context.font_metrics_depend_on_viewport_metrics,
        .root_font_metrics_depend_on_viewport_metrics = context.root_font_metrics_depend_on_viewport_metrics,
        .has_container_width_basis = false,
        .has_container_height_basis = false,
        .container_width_basis = 0,
        .container_height_basis = 0,
        .container_width_basis_depends_on_viewport_metrics = false,
        .container_height_basis_depends_on_viewport_metrics = false,
        .subject_inline_axis_is_horizontal = context.subject_inline_axis_is_horizontal,
        .resolved_viewport_relative_length = context.viewport_metric_dependency_flag(),
        .zoom = 1,
    };
}

WEB_API ComputedValuesFFI::FfiLengthResolutionContext to_ffi_length_resolution_context_with_container_bases(Length::ResolutionContext const&, u8 container_relative_length_unit_mask);

}
