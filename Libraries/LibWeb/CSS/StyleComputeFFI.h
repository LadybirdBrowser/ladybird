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

inline ComputedValuesFFI::FfiFontMetrics to_ffi_font_metrics(Length::FontMetrics const& metrics)
{
    return {
        .font_size = metrics.font_size.to_double(),
        .x_height = metrics.x_height.to_double(),
        .cap_height = metrics.cap_height.to_double(),
        .zero_advance = metrics.zero_advance.to_double(),
        .line_height = metrics.line_height.to_double(),
    };
}

inline ComputedValuesFFI::FfiLengthResolutionContext to_ffi_length_resolution_context(Length::ResolutionContext const& context)
{
    return {
        .viewport_width = context.viewport_rect.width().to_double(),
        .viewport_height = context.viewport_rect.height().to_double(),
        .font_metrics = to_ffi_font_metrics(context.font_metrics),
        .root_font_metrics = to_ffi_font_metrics(context.root_font_metrics),
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
    };
}

inline ComputedValuesFFI::FfiLengthResolutionContext to_ffi_length_resolution_context_with_container_bases(Length::ResolutionContext const& context, u8 container_relative_length_unit_mask)
{
    auto ffi_context = to_ffi_length_resolution_context(context);

    constexpr u8 cqw_mask = 1 << 0;
    constexpr u8 cqh_mask = 1 << 1;
    constexpr u8 cqi_mask = 1 << 2;
    constexpr u8 cqb_mask = 1 << 3;
    constexpr u8 cqmin_mask = 1 << 4;
    constexpr u8 cqmax_mask = 1 << 5;
    constexpr u8 both_axes_mask = cqmin_mask | cqmax_mask;
    auto const needs_width_basis = (container_relative_length_unit_mask & (cqw_mask | both_axes_mask | (context.subject_inline_axis_is_horizontal ? cqi_mask : cqb_mask))) != 0;
    auto const needs_height_basis = (container_relative_length_unit_mask & (cqh_mask | both_axes_mask | (context.subject_inline_axis_is_horizontal ? cqb_mask : cqi_mask))) != 0;

    if (needs_width_basis) {
        auto snapshot_context = context;
        snapshot_context.set_did_resolve_viewport_relative_length(ffi_context.container_width_basis_depends_on_viewport_metrics);
        ffi_context.container_width_basis = Length(100, LengthUnit::Cqw).to_px_without_rounding(snapshot_context);
        ffi_context.has_container_width_basis = true;
    }
    if (needs_height_basis) {
        auto snapshot_context = context;
        snapshot_context.set_did_resolve_viewport_relative_length(ffi_context.container_height_basis_depends_on_viewport_metrics);
        ffi_context.container_height_basis = Length(100, LengthUnit::Cqh).to_px_without_rounding(snapshot_context);
        ffi_context.has_container_height_basis = true;
    }

    return ffi_context;
}

}
