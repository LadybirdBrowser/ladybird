/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
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
        FontMetrics(CSSPixels font_size, Gfx::FontPixelMetrics const&);

        CSSPixels font_size;
        CSSPixels x_height;
        CSSPixels cap_height;
        CSSPixels zero_advance;
        CSSPixels line_height;

        bool operator==(FontMetrics const&) const = default;
    };

    Length(double value, LengthUnit unit);
    ~Length();

    static Length make_px(double value);
    static Length make_px(CSSPixels value);
    Length percentage_of(Percentage const&) const;

    bool is_px() const { return m_unit == LengthUnit::Px; }

    bool is_absolute() const { return CSS::is_absolute(m_unit); }
    bool is_font_relative() const { return CSS::is_font_relative(m_unit); }
    bool is_viewport_relative() const { return CSS::is_viewport_relative(m_unit); }
    bool is_relative() const { return CSS::is_relative(m_unit); }

    double raw_value() const { return m_value; }
    LengthUnit unit() const { return m_unit; }
    StringView unit_name() const { return CSS::to_string(m_unit); }

    struct ResolutionContext {
        [[nodiscard]] static ResolutionContext for_element(DOM::AbstractElement const&);
        [[nodiscard]] static ResolutionContext for_window(HTML::Window const&);
        [[nodiscard]] static ResolutionContext for_layout_node(Layout::Node const&);

        CSSPixelRect viewport_rect;
        FontMetrics font_metrics;
        FontMetrics root_font_metrics;

        bool operator==(ResolutionContext const&) const = default;
    };

    [[nodiscard]] CSSPixels to_px(ResolutionContext const&) const;

    [[nodiscard]] ALWAYS_INLINE CSSPixels to_px(Layout::Node const& node) const
    {
        if (is_absolute())
            return absolute_length_to_px();
        return to_px_slow_case(node);
    }

    ALWAYS_INLINE CSSPixels to_px(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const
    {
        if (is_absolute())
            return absolute_length_to_px();
        if (is_font_relative())
            return font_relative_length_to_px(font_metrics, root_font_metrics);
        if (is_viewport_relative())
            return viewport_relative_length_to_px(viewport_rect);

        VERIFY_NOT_REACHED();
    }

    ALWAYS_INLINE CSSPixels absolute_length_to_px() const
    {
        return CSSPixels::nearest_value_for(absolute_length_to_px_without_rounding());
    }

    ALWAYS_INLINE double absolute_length_to_px_without_rounding() const
    {
        constexpr double inch_pixels = 96.0;
        constexpr double centimeter_pixels = (inch_pixels / 2.54);

        switch (m_unit) {
        case LengthUnit::Cm:
            return m_value * centimeter_pixels; // 1cm = 96px/2.54
        case LengthUnit::In:
            return m_value * inch_pixels; // 1in = 2.54 cm = 96px
        case LengthUnit::Px:
            return m_value; // 1px = 1/96th of 1in
        case LengthUnit::Pt:
            return m_value * ((1.0 / 72.0) * inch_pixels); // 1pt = 1/72th of 1in
        case LengthUnit::Pc:
            return m_value * ((1.0 / 6.0) * inch_pixels); // 1pc = 1/6th of 1in
        case LengthUnit::Mm:
            return m_value * ((1.0 / 10.0) * centimeter_pixels); // 1mm = 1/10th of 1cm
        case LengthUnit::Q:
            return m_value * ((1.0 / 40.0) * centimeter_pixels); // 1Q = 1/40th of 1cm
        default:
            VERIFY_NOT_REACHED();
        }
    }

    String to_string(SerializationMode = SerializationMode::Normal) const;

    bool operator==(Length const& other) const
    {
        return m_unit == other.m_unit && m_value == other.m_value;
    }

    CSSPixels font_relative_length_to_px(FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    CSSPixels viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const;

    // Returns empty optional if it's already absolute.
    Optional<Length> absolutize(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    Length absolutized(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;

    static Length resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const&, Layout::Node const&, Length const& reference_value);
    static Length resolve_calculated(NonnullRefPtr<CalculatedStyleValue const> const&, Layout::Node const&, CSSPixels reference_value);

private:
    [[nodiscard]] CSSPixels to_px_slow_case(Layout::Node const&) const;

    LengthUnit m_unit;
    double m_value { 0 };
};

class LengthOrAuto {
public:
    LengthOrAuto(Length length)
        : m_length(move(length))
    {
    }

    static LengthOrAuto make_auto() { return LengthOrAuto { OptionalNone {} }; }

    bool is_length() const { return m_length.has_value(); }
    bool is_auto() const { return !m_length.has_value(); }

    Length const& length() const { return m_length.value(); }

    String to_string(SerializationMode mode = SerializationMode::Normal) const
    {
        if (is_auto())
            return "auto"_string;
        return m_length->to_string(mode);
    }

    CSSPixels to_px_or_zero(Layout::Node const& node) const
    {
        if (is_auto())
            return 0;
        return m_length->to_px(node);
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
