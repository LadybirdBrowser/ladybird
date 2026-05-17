/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

class WEB_API Length {
public:
    struct FontMetrics {
        FontMetrics(CSSPixels font_size, Gfx::FontPixelMetrics const&, CSSPixels line_height);

        CSSPixels font_size;
        CSSPixels x_height;
        CSSPixels cap_height;
        CSSPixels zero_advance;
        CSSPixels line_height;

        bool operator==(FontMetrics const&) const = default;
    };

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
    bool is_font_relative() const { return CSS::is_font_relative(m_unit); }
    bool is_container_relative() const { return CSS::is_container_relative(m_unit); }
    bool is_viewport_relative() const { return CSS::is_viewport_relative(m_unit); }
    bool is_relative() const { return CSS::is_relative(m_unit); }
    bool is_computationally_independent() const { return !is_font_relative() && !is_container_relative(); }

    double raw_value() const { return m_value; }
    LengthUnit unit() const { return m_unit; }
    FlyString unit_name() const { return CSS::to_string(m_unit); }

    struct ResolutionContext {
        [[nodiscard]] static ResolutionContext for_document(DOM::Document const&);
        [[nodiscard]] static ResolutionContext for_element(DOM::AbstractElement const&);
        [[nodiscard]] static ResolutionContext for_layout_node(Layout::Node const&);

        CSSPixelRect viewport_rect;
        FontMetrics font_metrics;
        FontMetrics root_font_metrics;
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

        void record_viewport_relative_length_resolution() const
        {
            if (m_did_resolve_viewport_relative_length)
                *m_did_resolve_viewport_relative_length = true;
        }

        void record_font_relative_length_resolution(LengthUnit unit) const
        {
            if (!m_did_resolve_viewport_relative_length)
                return;

            switch (unit) {
            case LengthUnit::Rem:
            case LengthUnit::Rex:
            case LengthUnit::Rcap:
            case LengthUnit::Rch:
            case LengthUnit::Ric:
            case LengthUnit::Rlh:
                if (root_font_metrics_depend_on_viewport_metrics)
                    *m_did_resolve_viewport_relative_length = true;
                return;
            case LengthUnit::Em:
            case LengthUnit::Ex:
            case LengthUnit::Cap:
            case LengthUnit::Ch:
            case LengthUnit::Ic:
            case LengthUnit::Lh:
                if (font_metrics_depend_on_viewport_metrics)
                    *m_did_resolve_viewport_relative_length = true;
                return;
            default:
                return;
            }
        }

        bool operator==(ResolutionContext const& other) const
        {
            return viewport_rect == other.viewport_rect
                && font_metrics == other.font_metrics
                && root_font_metrics == other.root_font_metrics
                && font_metrics_depend_on_viewport_metrics == other.font_metrics_depend_on_viewport_metrics
                && root_font_metrics_depend_on_viewport_metrics == other.root_font_metrics_depend_on_viewport_metrics
                && subject_inline_axis_is_horizontal == other.subject_inline_axis_is_horizontal
                && subject_element == other.subject_element;
        }

        void visit_edges(GC::Cell::Visitor&) const;

        mutable bool* m_did_resolve_viewport_relative_length { nullptr };
    };

    [[nodiscard]] CSSPixels to_px(ResolutionContext const&) const;

    [[nodiscard]] ALWAYS_INLINE CSSPixels to_px(Layout::Node const& node) const
    {
        if (is_absolute())
            return absolute_length_to_px();
        return to_px_slow_case(node);
    }

    ALWAYS_INLINE double to_px_without_rounding(ResolutionContext const& context) const
    {
        if (is_absolute())
            return absolute_length_to_px_without_rounding();
        if (is_font_relative()) {
            context.record_font_relative_length_resolution(m_unit);
            return font_relative_length_to_px_without_rounding(context.font_metrics, context.root_font_metrics);
        }
        if (is_viewport_relative()) {
            context.record_viewport_relative_length_resolution();
            return viewport_relative_length_to_px_without_rounding(context.viewport_rect);
        }
        if (is_container_relative())
            return container_relative_length_to_px_without_rounding(context);

        VERIFY_NOT_REACHED();
    }

    ALWAYS_INLINE CSSPixels to_px(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const
    {
        if (is_absolute())
            return absolute_length_to_px();
        if (is_font_relative())
            return font_relative_length_to_px(font_metrics, root_font_metrics);
        if (is_viewport_relative())
            return viewport_relative_length_to_px(viewport_rect);
        if (is_container_relative()) {
            ResolutionContext context {
                .viewport_rect = viewport_rect,
                .font_metrics = font_metrics,
                .root_font_metrics = root_font_metrics,
            };
            return CSSPixels::nearest_value_for(container_relative_length_to_px_without_rounding(context));
        }

        VERIFY_NOT_REACHED();
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
    String to_string(SerializationMode = SerializationMode::Normal) const;

    bool operator==(Length const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    CSSPixels font_relative_length_to_px(FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    double font_relative_length_to_px_without_rounding(FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    CSSPixels viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const;
    double viewport_relative_length_to_px_without_rounding(CSSPixelRect const& viewport_rect) const;
    double container_relative_length_to_px_without_rounding(ResolutionContext const&) const;

    // Returns empty optional if it's already absolute.
    Optional<Length> absolutize(ResolutionContext const&) const;

    static Length from_style_value(NonnullRefPtr<StyleValue const> const&, Optional<Length> percentage_basis);

private:
    [[nodiscard]] CSSPixels to_px_slow_case(Layout::Node const&) const;

    LengthUnit m_unit;
    double m_value { 0 };
};

// FIXME: This should be CSSPixelsOrAuto since it's only used after computation when lengths have been absolutized
class LengthOrAuto {
public:
    LengthOrAuto(Length length)
        : m_length(move(length))
    {
    }

    static LengthOrAuto make_auto() { return LengthOrAuto { OptionalNone {} }; }
    static LengthOrAuto from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<Length> percentage_basis);

    bool is_length() const { return m_length.has_value(); }
    bool is_auto() const { return !m_length.has_value(); }

    Length const& length() const { return m_length.value(); }

    void serialize(StringBuilder& builder, SerializationMode mode = SerializationMode::Normal) const
    {
        if (is_auto())
            builder.append("auto"sv);
        else
            m_length->serialize(builder, mode);
    }

    String to_string(SerializationMode mode = SerializationMode::Normal) const
    {
        StringBuilder builder;
        serialize(builder, mode);
        return builder.to_string_without_validation();
    }

    CSSPixels to_px_or_zero() const
    {
        if (is_auto())
            return 0;
        return m_length->absolute_length_to_px();
    }

    bool operator==(LengthOrAuto const&) const = default;

private:
    explicit LengthOrAuto(Optional<Length> maybe_length)
        : m_length(move(maybe_length))
    {
    }

    Optional<Length> m_length;
};

}

template<>
struct AK::Formatter<Web::CSS::Length> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Length const& length)
    {
        return Formatter<StringView>::format(builder, length.to_string());
    }
};

template<>
struct AK::Formatter<Web::CSS::LengthOrAuto> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::LengthOrAuto const& length_or_auto)
    {
        return Formatter<StringView>::format(builder, length_or_auto.to_string());
    }
};
