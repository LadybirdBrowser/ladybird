/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>
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

class WEB_API Length {
public:
    Length(double value, LengthUnit unit)
        : m_unit(unit)
        , m_value(value)
    {
    }
    ~Length() = default;

    [[nodiscard]] static Length make_px(double value) { return Length(value, LengthUnit::Px); }
    [[nodiscard]] static Length make_px(CSSPixels value) { return make_px(value.to_double()); }
    Length percentage_of(Percentage const&) const;

    bool is_px() const { return m_unit == LengthUnit::Px; }

    bool is_absolute() const { return CSS::is_absolute(m_unit); }
    bool is_container_relative() const { return CSS::is_container_relative(m_unit); }

    double raw_value() const { return m_value; }
    LengthUnit unit() const { return m_unit; }
    Utf16FlyString unit_name() const { return CSS::to_string(m_unit); }

    struct ResolutionContext {
        [[nodiscard]] static ResolutionContext for_document(DOM::Document const&);
        [[nodiscard]] static ResolutionContext for_element(DOM::AbstractElement const&);
        [[nodiscard]] static ResolutionContext for_element(DOM::AbstractElement const&, ComputedValues const&);

        CSSPixelRect viewport_rect;
        ComputedValuesFFI::FfiFontMetrics font_metrics;
        ComputedValuesFFI::FfiFontMetrics root_font_metrics;
        bool font_metrics_depend_on_viewport_metrics { false };
        bool root_font_metrics_depend_on_viewport_metrics { false };
        bool subject_inline_axis_is_horizontal { true };
        GC::Ptr<DOM::Element const> subject_element { nullptr };
        mutable Optional<GC::Ptr<DOM::Element const>> cached_width_query_container {};
        mutable Optional<GC::Ptr<DOM::Element const>> cached_height_query_container {};

        void set_did_resolve_viewport_relative_length(bool& did_resolve_viewport_relative_length) const
        {
            m_did_resolve_viewport_relative_length = &did_resolve_viewport_relative_length;
        }

        [[nodiscard]] bool* viewport_metric_dependency_flag() const
        {
            return m_did_resolve_viewport_relative_length;
        }

        void record_viewport_relative_length_resolution() const
        {
            if (m_did_resolve_viewport_relative_length)
                *m_did_resolve_viewport_relative_length = true;
        }

        void visit_edges(GC::Cell::Visitor&) const;

        mutable bool* m_did_resolve_viewport_relative_length { nullptr };
    };

    ALWAYS_INLINE double to_px_without_rounding(ResolutionContext const& context) const
    {
        VERIFY(is_container_relative());
        return container_relative_length_to_px_without_rounding(context);
    }

    [[nodiscard]] ALWAYS_INLINE CSSPixels absolute_length_to_px() const
    {
        return CSSPixels::nearest_value_for(absolute_length_to_px_without_rounding());
    }

    [[nodiscard]] ALWAYS_INLINE double absolute_length_to_px_without_rounding() const
    {
        return ratio_between_units(m_unit, LengthUnit::Px) * m_value;
    }

    void serialize(StringBuilder&, SerializationMode = SerializationMode::Normal) const;
    void serialize(Utf16StringBuilder&, SerializationMode = SerializationMode::Normal) const;

    bool operator==(Length const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    double container_relative_length_to_px_without_rounding(ResolutionContext const&) const;

    static Length from_style_value(NonnullRefPtr<StyleValue const> const&, Optional<Length> percentage_basis);

private:
    LengthUnit m_unit;
    double m_value { 0 };
};

}
