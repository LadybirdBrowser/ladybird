/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::CSS {

namespace ComputedValuesFFI {

inline bool operator==(FfiFontMetrics const& a, FfiFontMetrics const& b)
{
    return a.font_size == b.font_size && a.x_height == b.x_height && a.cap_height == b.cap_height && a.zero_advance == b.zero_advance && a.line_height == b.line_height;
}

}

WEB_API ComputedValuesFFI::FfiFontMetrics font_metrics_from(CSSPixels font_size, Gfx::FontPixelMetrics const&, CSSPixels line_height);

inline constexpr u8 all_container_relative_length_units_mask = (1 << 6) - 1;

struct WEB_API LengthResolutionContext {
    [[nodiscard]] static LengthResolutionContext for_document(DOM::Document const&);
    [[nodiscard]] static LengthResolutionContext for_element(DOM::AbstractElement const&);
    [[nodiscard]] static LengthResolutionContext for_element(DOM::AbstractElement const&, ComputedValues const&);

    CSSPixelRect viewport_rect;
    ComputedValuesFFI::FfiFontMetrics font_metrics;
    ComputedValuesFFI::FfiFontMetrics root_font_metrics;
    bool font_metrics_depend_on_viewport_metrics { false };
    bool root_font_metrics_depend_on_viewport_metrics { false };
    bool subject_inline_axis_is_horizontal { true };
    GC::Ptr<DOM::Element const> subject_element { nullptr };

    [[nodiscard]] ComputedValuesFFI::FfiLengthResolutionContext to_ffi() const
    {
        return {
            .viewport_width = viewport_rect.width().to_double(),
            .viewport_height = viewport_rect.height().to_double(),
            .font_metrics = font_metrics,
            .root_font_metrics = root_font_metrics,
            .font_metrics_depend_on_viewport_metrics = font_metrics_depend_on_viewport_metrics,
            .root_font_metrics_depend_on_viewport_metrics = root_font_metrics_depend_on_viewport_metrics,
            .has_container_width_basis = false,
            .has_container_height_basis = false,
            .container_width_basis = 0,
            .container_height_basis = 0,
            .container_width_basis_depends_on_viewport_metrics = false,
            .container_height_basis_depends_on_viewport_metrics = false,
            .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
            .resolved_viewport_relative_length = m_did_resolve_viewport_relative_length,
            .zoom = 1,
        };
    }

    [[nodiscard]] ComputedValuesFFI::FfiLengthResolutionContext to_ffi_with_container_bases(u8 container_relative_length_unit_mask) const;

    void set_did_resolve_viewport_relative_length(bool& did_resolve_viewport_relative_length) const
    {
        m_did_resolve_viewport_relative_length = &did_resolve_viewport_relative_length;
    }

    void visit_edges(GC::Cell::Visitor&) const;

    mutable bool* m_did_resolve_viewport_relative_length { nullptr };
};

}
