/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibGfx/SystemTheme.h>

namespace Gfx {

class PaletteImpl : public RefCounted<PaletteImpl> {
    AK_MAKE_NONCOPYABLE(PaletteImpl);
    AK_MAKE_NONMOVABLE(PaletteImpl);

public:
    ~PaletteImpl() = default;
    static NonnullRefPtr<PaletteImpl> create_with_anonymous_buffer(Core::AnonymousBuffer);
    NonnullRefPtr<PaletteImpl> clone() const;

    Color color(ColorRole role) const
    {
        VERIFY((int)role < (int)ColorRole::__Count);
        return Color::from_argb(theme().color[(int)role]);
    }

    Gfx::TextAlignment alignment(AlignmentRole role) const
    {
        VERIFY((int)role < (int)AlignmentRole::__Count);
        return theme().alignment[(int)role];
    }

    bool flag(FlagRole role) const
    {
        VERIFY((int)role < (int)FlagRole::__Count);
        return theme().flag[(int)role];
    }

    int metric(MetricRole) const;
    ByteString path(PathRole) const;
    SystemTheme const& theme() const { return *m_theme_buffer.data<SystemTheme>(); }

    void replace_internal_buffer(Core::AnonymousBuffer);

private:
    explicit PaletteImpl(Core::AnonymousBuffer);

    Core::AnonymousBuffer m_theme_buffer;
};

class Palette {

public:
    explicit Palette(NonnullRefPtr<PaletteImpl>);
    ~Palette() = default;

    Color accent() const { return color(ColorRole::Accent); }
    Color window() const { return color(ColorRole::Window); }
    Color window_text() const { return color(ColorRole::WindowText); }
    Color selection() const { return color(ColorRole::Selection); }
    Color selection_text() const { return color(ColorRole::SelectionText); }
    Color inactive_selection() const { return color(ColorRole::InactiveSelection); }
    Color inactive_selection_text() const { return color(ColorRole::InactiveSelectionText); }
    Color desktop_background() const { return color(ColorRole::DesktopBackground); }
    Color active_window_border1() const { return color(ColorRole::ActiveWindowBorder1); }
    Color active_window_border2() const { return color(ColorRole::ActiveWindowBorder2); }
    Color active_window_title() const { return color(ColorRole::ActiveWindowTitle); }
    Color active_window_title_stripes() const { return color(ColorRole::ActiveWindowTitleStripes); }
    Color active_window_title_shadow() const { return color(ColorRole::ActiveWindowTitleShadow); }
    Color inactive_window_border1() const { return color(ColorRole::InactiveWindowBorder1); }
    Color inactive_window_border2() const { return color(ColorRole::InactiveWindowBorder2); }
    Color inactive_window_title() const { return color(ColorRole::InactiveWindowTitle); }
    Color inactive_window_title_stripes() const { return color(ColorRole::InactiveWindowTitleStripes); }
    Color inactive_window_title_shadow() const { return color(ColorRole::InactiveWindowTitleShadow); }
    Color moving_window_border1() const { return color(ColorRole::MovingWindowBorder1); }
    Color moving_window_border2() const { return color(ColorRole::MovingWindowBorder2); }
    Color moving_window_title() const { return color(ColorRole::MovingWindowTitle); }
    Color moving_window_title_stripes() const { return color(ColorRole::MovingWindowTitleStripes); }
    Color moving_window_title_shadow() const { return color(ColorRole::MovingWindowTitleShadow); }
    Color highlight_window_border1() const { return color(ColorRole::HighlightWindowBorder1); }
    Color highlight_window_border2() const { return color(ColorRole::HighlightWindowBorder2); }
    Color highlight_window_title() const { return color(ColorRole::HighlightWindowTitle); }
    Color highlight_window_title_stripes() const { return color(ColorRole::HighlightWindowTitleStripes); }
    Color highlight_window_title_shadow() const { return color(ColorRole::HighlightWindowTitleShadow); }
    Color highlight_searching() const { return color(ColorRole::HighlightSearching); }
    Color highlight_searching_text() const { return color(ColorRole::HighlightSearchingText); }
    Color menu_stripe() const { return color(ColorRole::MenuStripe); }
    Color menu_base() const { return color(ColorRole::MenuBase); }
    Color menu_base_text() const { return color(ColorRole::MenuBaseText); }
    Color menu_selection() const { return color(ColorRole::MenuSelection); }
    Color menu_selection_text() const { return color(ColorRole::MenuSelectionText); }
    Color base() const { return color(ColorRole::Base); }
    Color base_text() const { return color(ColorRole::BaseText); }
    Color disabled_text_front() const { return color(ColorRole::DisabledTextFront); }
    Color disabled_text_back() const { return color(ColorRole::DisabledTextBack); }
    Color button() const { return color(ColorRole::Button); }
    Color button_text() const { return color(ColorRole::ButtonText); }
    Color threed_highlight() const { return color(ColorRole::ThreedHighlight); }
    Color threed_shadow1() const { return color(ColorRole::ThreedShadow1); }
    Color threed_shadow2() const { return color(ColorRole::ThreedShadow2); }
    Color hover_highlight() const { return color(ColorRole::HoverHighlight); }
    Color rubber_band_fill() const { return color(ColorRole::RubberBandFill); }
    Color rubber_band_border() const { return color(ColorRole::RubberBandBorder); }
    Color gutter() const { return color(ColorRole::Gutter); }
    Color gutter_border() const { return color(ColorRole::GutterBorder); }
    Color ruler() const { return color(ColorRole::Ruler); }
    Color ruler_border() const { return color(ColorRole::RulerBorder); }
    Color ruler_active_text() const { return color(ColorRole::RulerActiveText); }
    Color ruler_inactive_text() const { return color(ColorRole::RulerInactiveText); }
    Color text_cursor() const { return color(ColorRole::TextCursor); }
    Color focus_outline() const { return color(ColorRole::FocusOutline); }
    Color tray() const { return color(ColorRole::Tray); }
    Color tray_text() const { return color(ColorRole::TrayText); }

    Color link() const { return color(ColorRole::Link); }
    Color active_link() const { return color(ColorRole::ActiveLink); }
    Color visited_link() const { return color(ColorRole::VisitedLink); }

    Color syntax_comment() const { return color(ColorRole::SyntaxComment); }
    Color syntax_number() const { return color(ColorRole::SyntaxNumber); }
    Color syntax_string() const { return color(ColorRole::SyntaxString); }
    Color syntax_identifier() const { return color(ColorRole::SyntaxIdentifier); }
    Color syntax_type() const { return color(ColorRole::SyntaxType); }
    Color syntax_punctuation() const { return color(ColorRole::SyntaxPunctuation); }
    Color syntax_operator() const { return color(ColorRole::SyntaxOperator); }
    Color syntax_keyword() const { return color(ColorRole::SyntaxKeyword); }
    Color syntax_control_keyword() const { return color(ColorRole::SyntaxControlKeyword); }
    Color syntax_preprocessor_statement() const { return color(ColorRole::SyntaxPreprocessorStatement); }
    Color syntax_preprocessor_value() const { return color(ColorRole::SyntaxPreprocessorValue); }
    Color syntax_function() const { return color(ColorRole::SyntaxFunction); }
    Color syntax_variable() const { return color(ColorRole::SyntaxVariable); }
    Color syntax_custom_type() const { return color(ColorRole::SyntaxCustomType); }
    Color syntax_namespace() const { return color(ColorRole::SyntaxNamespace); }
    Color syntax_member() const { return color(ColorRole::SyntaxMember); }
    Color syntax_parameter() const { return color(ColorRole::SyntaxParameter); }

    Color background() const { return color(ColorRole::ColorSchemeBackground); }
    Color foreground() const { return color(ColorRole::ColorSchemeForeground); }

    Color black() const { return color(ColorRole::Black); }
    Color red() const { return color(ColorRole::Red); }
    Color green() const { return color(ColorRole::Green); }
    Color yellow() const { return color(ColorRole::Yellow); }
    Color blue() const { return color(ColorRole::Blue); }
    Color magenta() const { return color(ColorRole::Magenta); }
    Color cyan() const { return color(ColorRole::Cyan); }
    Color white() const { return color(ColorRole::White); }

    Color bright_black() const { return color(ColorRole::BrightBlack); }
    Color bright_red() const { return color(ColorRole::BrightRed); }
    Color bright_green() const { return color(ColorRole::BrightGreen); }
    Color bright_yellow() const { return color(ColorRole::BrightYellow); }
    Color bright_blue() const { return color(ColorRole::BrightBlue); }
    Color bright_magenta() const { return color(ColorRole::BrightMagenta); }
    Color bright_cyan() const { return color(ColorRole::BrightCyan); }
    Color bright_white() const { return color(ColorRole::BrightWhite); }

    Gfx::TextAlignment title_alignment() const { return alignment(AlignmentRole::TitleAlignment); }

    bool bold_text_as_bright() const { return flag(FlagRole::BoldTextAsBright); }
    bool is_dark() const { return flag(FlagRole::IsDark); }
    bool title_buttons_icon_only() const { return flag(FlagRole::TitleButtonsIconOnly); }

    int window_border_thickness() const { return metric(MetricRole::BorderThickness); }
    int window_border_radius() const { return metric(MetricRole::BorderRadius); }
    int window_title_height() const { return metric(MetricRole::TitleHeight); }
    int window_title_button_width() const { return metric(MetricRole::TitleButtonWidth); }
    int window_title_button_height() const { return metric(MetricRole::TitleButtonHeight); }

    ByteString title_button_icons_path() const { return path(PathRole::TitleButtonIcons); }
    ByteString active_window_shadow_path() const { return path(PathRole::ActiveWindowShadow); }
    ByteString inactive_window_shadow_path() const { return path(PathRole::InactiveWindowShadow); }
    ByteString menu_shadow_path() const { return path(PathRole::MenuShadow); }
    ByteString taskbar_shadow_path() const { return path(PathRole::TaskbarShadow); }
    ByteString tooltip_shadow_path() const { return path(PathRole::TooltipShadow); }
    ByteString color_scheme_path() const { return path(PathRole::ColorScheme); }

    Color color(ColorRole role) const { return m_impl->color(role); }
    Gfx::TextAlignment alignment(AlignmentRole role) const { return m_impl->alignment(role); }
    bool flag(FlagRole role) const { return m_impl->flag(role); }
    int metric(MetricRole role) const { return m_impl->metric(role); }
    ByteString path(PathRole role) const { return m_impl->path(role); }

    void set_color(ColorRole, Color);
    void set_alignment(AlignmentRole, Gfx::TextAlignment);
    void set_flag(FlagRole, bool);
    void set_metric(MetricRole, int);
    void set_path(PathRole, ByteString);

    SystemTheme const& theme() const { return m_impl->theme(); }

    PaletteImpl& impl() { return *m_impl; }
    PaletteImpl const& impl() const { return *m_impl; }

private:
    NonnullRefPtr<PaletteImpl> m_impl;
};

}

using Gfx::Palette;
