/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>

namespace Line {

class Style {
public:
    bool operator==(Style const&) const = default;

    enum class XtermColor : int {
        Default = 9,
        Black = 0,
        Red,
        Green,
        Yellow,
        Blue,
        Magenta,
        Cyan,
        White,
        Unchanged,
    };

    struct AnchoredTag {
    };
    struct UnderlineTag {
    };
    struct BoldTag {
    };
    struct ItalicTag {
    };
    struct Color {
        bool operator==(Color const&) const = default;

        explicit Color(XtermColor color)
            : m_xterm_color(color)
            , m_is_rgb(false)
        {
        }
        Color(u8 r, u8 g, u8 b)
            : m_rgb_color({ r, g, b })
            , m_is_rgb(true)
        {
        }

        bool is_default() const
        {
            return !m_is_rgb && m_xterm_color == XtermColor::Unchanged;
        }

        XtermColor m_xterm_color { XtermColor::Unchanged };
        Vector<int, 3> m_rgb_color;
        bool m_is_rgb { false };
    };

    struct Background : public Color {
        explicit Background(XtermColor color)
            : Color(color)
        {
        }
        Background(u8 r, u8 g, u8 b)
            : Color(r, g, b)
        {
        }
        ByteString to_vt_escape() const;
    };

    struct Foreground : public Color {
        explicit Foreground(XtermColor color)
            : Color(color)
        {
        }
        Foreground(u8 r, u8 g, u8 b)
            : Color(r, g, b)
        {
        }

        ByteString to_vt_escape() const;
    };

    struct Hyperlink {
        bool operator==(Hyperlink const&) const = default;

        explicit Hyperlink(StringView link)
            : m_link(link)
        {
            m_has_link = true;
        }

        Hyperlink() = default;

        ByteString to_vt_escape(bool starting) const;

        bool is_empty() const { return !m_has_link; }

        ByteString m_link;
        bool m_has_link { false };
    };

    struct Mask {
        bool operator==(Mask const& other) const
        {
            return other.mode == mode && other.replacement == replacement;
        }

        enum class Mode {
            ReplaceEntireSelection,
            ReplaceEachCodePointInSelection,
        };
        explicit Mask(StringView replacement, Mode mode = Mode::ReplaceEntireSelection)
            : replacement(replacement)
            , replacement_view(this->replacement)
            , mode(mode)
        {
        }

        ByteString replacement;
        mutable Utf8View replacement_view;
        Mode mode;
    };

    static constexpr UnderlineTag Underline {};
    static constexpr BoldTag Bold {};
    static constexpr ItalicTag Italic {};
    static constexpr AnchoredTag Anchored {};

    // Prepare for the horror of templates.
    template<typename T, typename... Rest>
    Style(T const& style_arg, Rest... rest)
        : Style(rest...)
    {
        set(style_arg);
        m_is_empty = false;
    }
    Style() = default;

    static Style reset_style()
    {
        return { Foreground(XtermColor::Default), Background(XtermColor::Default), Hyperlink(""sv) };
    }

    Style unified_with(Style const& other, bool prefer_other = true) const
    {
        Style style = *this;
        style.unify_with(other, prefer_other);
        return style;
    }

    void unify_with(Style const&, bool prefer_other = false);

    bool underline() const { return m_underline; }
    bool bold() const { return m_bold; }
    bool italic() const { return m_italic; }
    Background background() const { return m_background; }
    Foreground foreground() const { return m_foreground; }
    Hyperlink hyperlink() const { return m_hyperlink; }
    Optional<Mask> mask() const { return m_mask; }

    void unset_mask() const { m_mask = {}; }

    void set(ItalicTag const&) { m_italic = true; }
    void set(BoldTag const&) { m_bold = true; }
    void set(UnderlineTag const&) { m_underline = true; }
    void set(Background const& bg) { m_background = bg; }
    void set(Foreground const& fg) { m_foreground = fg; }
    void set(Hyperlink const& link) { m_hyperlink = link; }
    void set(AnchoredTag const&) { m_is_anchored = true; }
    void set(Mask const& mask) { m_mask = mask; }

    bool is_anchored() const { return m_is_anchored; }
    bool is_empty() const { return m_is_empty; }

    ByteString to_byte_string() const;

private:
    bool m_underline { false };
    bool m_bold { false };
    bool m_italic { false };
    Background m_background { XtermColor::Unchanged };
    Foreground m_foreground { XtermColor::Unchanged };
    Hyperlink m_hyperlink;
    mutable Optional<Mask> m_mask;

    bool m_is_anchored { false };

    bool m_is_empty { true };
};
}
