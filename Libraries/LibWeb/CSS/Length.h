/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

class Length {
public:
    enum class Type {
        // Font-relative
        Em,
        Rem,
        Ex,
        Rex,
        Cap,
        Rcap,
        Ch,
        Rch,
        Ic,
        Ric,
        Lh,
        Rlh,

        // Viewport-relative
        Vw,
        Svw,
        Lvw,
        Dvw,
        Vh,
        Svh,
        Lvh,
        Dvh,
        Vi,
        Svi,
        Lvi,
        Dvi,
        Vb,
        Svb,
        Lvb,
        Dvb,
        Vmin,
        Svmin,
        Lvmin,
        Dvmin,
        Vmax,
        Svmax,
        Lvmax,
        Dvmax,

        // Absolute
        Cm,
        Mm,
        Q,
        In,
        Pt,
        Pc,
        Px,

        // FIXME: Remove auto somehow
        Auto,
    };

    struct FontMetrics {
        FontMetrics(CSSPixels font_size, Gfx::FontPixelMetrics const&);

        CSSPixels font_size;
        CSSPixels x_height;
        CSSPixels cap_height;
        CSSPixels zero_advance;
        CSSPixels line_height;
    };

    static Optional<Type> unit_from_name(StringView);

    Length(double value, Type type);
    ~Length();

    static Length make_auto();
    static Length make_px(CSSPixels value);
    Length percentage_of(Percentage const&) const;

    bool is_auto() const { return m_type == Type::Auto; }
    bool is_px() const { return m_type == Type::Px; }

    bool is_absolute() const
    {
        return m_type == Type::Cm
            || m_type == Type::Mm
            || m_type == Type::Q
            || m_type == Type::In
            || m_type == Type::Pt
            || m_type == Type::Pc
            || m_type == Type::Px;
    }

    bool is_font_relative() const
    {
        return m_type == Type::Em
            || m_type == Type::Rem
            || m_type == Type::Ex
            || m_type == Type::Rex
            || m_type == Type::Cap
            || m_type == Type::Rcap
            || m_type == Type::Ch
            || m_type == Type::Rch
            || m_type == Type::Ic
            || m_type == Type::Ric
            || m_type == Type::Lh
            || m_type == Type::Rlh;
    }

    bool is_viewport_relative() const
    {
        return m_type == Type::Vw
            || m_type == Type::Svw
            || m_type == Type::Lvw
            || m_type == Type::Dvw
            || m_type == Type::Vh
            || m_type == Type::Svh
            || m_type == Type::Lvh
            || m_type == Type::Dvh
            || m_type == Type::Vi
            || m_type == Type::Svi
            || m_type == Type::Lvi
            || m_type == Type::Dvi
            || m_type == Type::Vb
            || m_type == Type::Svb
            || m_type == Type::Lvb
            || m_type == Type::Dvb
            || m_type == Type::Vmin
            || m_type == Type::Svmin
            || m_type == Type::Lvmin
            || m_type == Type::Dvmin
            || m_type == Type::Vmax
            || m_type == Type::Svmax
            || m_type == Type::Lvmax
            || m_type == Type::Dvmax;
    }

    bool is_relative() const
    {
        return is_font_relative() || is_viewport_relative();
    }

    Type type() const { return m_type; }
    double raw_value() const { return m_value; }
    StringView unit_name() const;

    struct ResolutionContext {
        [[nodiscard]] static Length::ResolutionContext for_layout_node(Layout::Node const&);

        CSSPixelRect viewport_rect;
        FontMetrics font_metrics;
        FontMetrics root_font_metrics;
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
        if (is_auto())
            return 0;
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
        constexpr double inch_pixels = 96.0;
        constexpr double centimeter_pixels = (inch_pixels / 2.54);
        switch (m_type) {
        case Type::Cm:
            return CSSPixels::nearest_value_for(m_value * centimeter_pixels); // 1cm = 96px/2.54
        case Type::In:
            return CSSPixels::nearest_value_for(m_value * inch_pixels); // 1in = 2.54 cm = 96px
        case Type::Px:
            return CSSPixels::nearest_value_for(m_value); // 1px = 1/96th of 1in
        case Type::Pt:
            return CSSPixels::nearest_value_for(m_value * ((1.0 / 72.0) * inch_pixels)); // 1pt = 1/72th of 1in
        case Type::Pc:
            return CSSPixels::nearest_value_for(m_value * ((1.0 / 6.0) * inch_pixels)); // 1pc = 1/6th of 1in
        case Type::Mm:
            return CSSPixels::nearest_value_for(m_value * ((1.0 / 10.0) * centimeter_pixels)); // 1mm = 1/10th of 1cm
        case Type::Q:
            return CSSPixels::nearest_value_for(m_value * ((1.0 / 40.0) * centimeter_pixels)); // 1Q = 1/40th of 1cm
        default:
            VERIFY_NOT_REACHED();
        }
    }

    String to_string() const;

    bool operator==(Length const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
    }

    CSSPixels font_relative_length_to_px(FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    CSSPixels viewport_relative_length_to_px(CSSPixelRect const& viewport_rect) const;

    // Returns empty optional if it's already absolute.
    Optional<Length> absolutize(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;
    Length absolutized(CSSPixelRect const& viewport_rect, FontMetrics const& font_metrics, FontMetrics const& root_font_metrics) const;

    static Length resolve_calculated(NonnullRefPtr<CSSMathValue> const&, Layout::Node const&, Length const& reference_value);
    static Length resolve_calculated(NonnullRefPtr<CSSMathValue> const&, Layout::Node const&, CSSPixels reference_value);

private:
    [[nodiscard]] CSSPixels to_px_slow_case(Layout::Node const&) const;

    Type m_type;
    double m_value { 0 };
};

}

template<>
struct AK::Formatter<Web::CSS::Length> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Length const& length)
    {
        return Formatter<StringView>::format(builder, length.to_string());
    }
};
