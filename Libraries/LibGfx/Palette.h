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

private:
    explicit PaletteImpl(Core::AnonymousBuffer);

    Core::AnonymousBuffer m_theme_buffer;
};

class Palette {

public:
    explicit Palette(NonnullRefPtr<PaletteImpl>);
    ~Palette() = default;

    Color base() const { return color(ColorRole::Base); }
    Color base_text() const { return color(ColorRole::BaseText); }
    Color threed_shadow1() const { return color(ColorRole::ThreedShadow1); }

    Color syntax_comment() const { return color(ColorRole::SyntaxComment); }
    Color syntax_number() const { return color(ColorRole::SyntaxNumber); }
    Color syntax_string() const { return color(ColorRole::SyntaxString); }
    Color syntax_identifier() const { return color(ColorRole::SyntaxIdentifier); }
    Color syntax_punctuation() const { return color(ColorRole::SyntaxPunctuation); }
    Color syntax_operator() const { return color(ColorRole::SyntaxOperator); }
    Color syntax_keyword() const { return color(ColorRole::SyntaxKeyword); }
    Color syntax_control_keyword() const { return color(ColorRole::SyntaxControlKeyword); }
    Color syntax_preprocessor_statement() const { return color(ColorRole::SyntaxPreprocessorStatement); }

    bool is_dark() const { return flag(FlagRole::IsDark); }

    Color color(ColorRole role) const { return m_impl->color(role); }
    Gfx::TextAlignment alignment(AlignmentRole role) const { return m_impl->alignment(role); }
    bool flag(FlagRole role) const { return m_impl->flag(role); }
    int metric(MetricRole role) const { return m_impl->metric(role); }
    ByteString path(PathRole role) const { return m_impl->path(role); }

    void set_color(ColorRole, Color);
    void set_flag(FlagRole, bool);

    PaletteImpl& impl() { return *m_impl; }
    PaletteImpl const& impl() const { return *m_impl; }

private:
    NonnullRefPtr<PaletteImpl> m_impl;
};

}

using Gfx::Palette;
