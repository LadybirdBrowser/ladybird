/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Font.h>
#include <LibWeb/CSS/ComputedValues.h>

namespace Web::Layout {

// https://drafts.csswg.org/css-inline/#dominant-baseline-property
static float dominant_baseline_offset(CSS::BaselineMetric metric, Gfx::FontPixelMetrics const& font_metrics)
{
    switch (metric) {
    case CSS::BaselineMetric::Central:
        return (font_metrics.ascent - font_metrics.descent) / 2;
    case CSS::BaselineMetric::Middle:
        return font_metrics.x_height / 2;
    case CSS::BaselineMetric::Hanging:
        // FIXME: Read the hanging baseline from the font's BASE table.
        return font_metrics.ascent * 0.8f;
    case CSS::BaselineMetric::Ideographic:
        // FIXME: Read the ideographic baseline from the font's BASE table.
        return -font_metrics.descent;
    case CSS::BaselineMetric::Mathematical:
        // FIXME: Read the math baseline from the font's BASE table.
        return font_metrics.ascent * 0.5f;
    case CSS::BaselineMetric::TextTop:
    case CSS::BaselineMetric::TextBottom:
        // FIXME: Support text-top and text-bottom.
    case CSS::BaselineMetric::Alphabetic:
        return 0;
    }
    VERIFY_NOT_REACHED();
}

static CSS::BaselineMetric resolve_dominant_baseline_metric(CSS::ComputedValues const& computed_values)
{
    auto dominant_baseline = computed_values.dominant_baseline();
    if (dominant_baseline.has_value())
        return *dominant_baseline;

    // https://drafts.csswg.org/css-inline/#valdef-dominant-baseline-auto
    // Equivalent to alphabetic in horizontal writing modes and in vertical writing modes when text-orientation is
    // sideways. Equivalent to central in vertical writing modes when text-orientation is mixed or upright.
    // FIXME: Take text-orientation into account once it is implemented.
    switch (computed_values.writing_mode()) {
    case CSS::WritingMode::HorizontalTb:
    case CSS::WritingMode::SidewaysRl:
    case CSS::WritingMode::SidewaysLr:
        return CSS::BaselineMetric::Alphabetic;
    case CSS::WritingMode::VerticalRl:
    case CSS::WritingMode::VerticalLr:
        return CSS::BaselineMetric::Central;
    }
    VERIFY_NOT_REACHED();
}

}
