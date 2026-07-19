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
    };
}

}
