/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
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
        return Color::from_bgra(theme().color[(int)role]);
    }

    bool flag(FlagRole role) const
    {
        VERIFY((int)role < (int)FlagRole::__Count);
        return theme().flag[(int)role];
    }

    SystemTheme const& theme() const { return *m_theme_buffer.data<SystemTheme>(); }

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
    Color selection() const { return color(ColorRole::Selection); }
    Color inactive_selection() const { return color(ColorRole::InactiveSelection); }
    Color light_selection() const { return color(ColorRole::LightSelection); }
    Color dark_selection() const { return color(ColorRole::DarkSelection); }
    Color base() const { return color(ColorRole::Base); }
    Color base_text() const { return color(ColorRole::BaseText); }
    Color button() const { return color(ColorRole::Button); }
    Color button_text() const { return color(ColorRole::ButtonText); }
    Color threed_shadow1() const { return color(ColorRole::ThreedShadow1); }
    Color gutter() const { return color(ColorRole::Gutter); }

    Color link() const { return color(ColorRole::Link); }

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

    bool is_dark() const { return flag(FlagRole::IsDark); }

    Color color(ColorRole role) const { return m_impl->color(role); }
    bool flag(FlagRole role) const { return m_impl->flag(role); }

    void set_color(ColorRole, Color);
    void set_flag(FlagRole, bool);

    SystemTheme const& theme() const { return m_impl->theme(); }

    PaletteImpl& impl() { return *m_impl; }
    PaletteImpl const& impl() const { return *m_impl; }

private:
    NonnullRefPtr<PaletteImpl> m_impl;
};

}

using Gfx::Palette;
