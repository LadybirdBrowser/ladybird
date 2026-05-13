/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ConfigFile.h>
#include <LibGfx/Color.h>

namespace Gfx {

#define ENUMERATE_COLOR_ROLES(C)   \
    C(Accent)                      \
    C(ActiveLink)                  \
    C(ActiveWindowBorder1)         \
    C(ActiveWindowBorder2)         \
    C(ActiveWindowTitle)           \
    C(ActiveWindowTitleShadow)     \
    C(ActiveWindowTitleStripes)    \
    C(Base)                        \
    C(BaseText)                    \
    C(Black)                       \
    C(Blue)                        \
    C(BrightBlack)                 \
    C(BrightBlue)                  \
    C(BrightCyan)                  \
    C(BrightGreen)                 \
    C(BrightMagenta)               \
    C(BrightRed)                   \
    C(BrightWhite)                 \
    C(BrightYellow)                \
    C(Button)                      \
    C(ButtonText)                  \
    C(ColorSchemeBackground)       \
    C(ColorSchemeForeground)       \
    C(Cyan)                        \
    C(DisabledTextFront)           \
    C(DisabledTextBack)            \
    C(DesktopBackground)           \
    C(FocusOutline)                \
    C(Green)                       \
    C(Gutter)                      \
    C(GutterBorder)                \
    C(HighlightWindowBorder1)      \
    C(HighlightWindowBorder2)      \
    C(HighlightWindowTitle)        \
    C(HighlightWindowTitleShadow)  \
    C(HighlightWindowTitleStripes) \
    C(HighlightSearching)          \
    C(HighlightSearchingText)      \
    C(HoverHighlight)              \
    C(InactiveSelection)           \
    C(InactiveSelectionText)       \
    C(InactiveWindowBorder1)       \
    C(InactiveWindowBorder2)       \
    C(InactiveWindowTitle)         \
    C(InactiveWindowTitleShadow)   \
    C(InactiveWindowTitleStripes)  \
    C(Link)                        \
    C(Magenta)                     \
    C(MenuBase)                    \
    C(MenuBaseText)                \
    C(MenuSelection)               \
    C(MenuSelectionText)           \
    C(MenuStripe)                  \
    C(MovingWindowBorder1)         \
    C(MovingWindowBorder2)         \
    C(MovingWindowTitle)           \
    C(MovingWindowTitleShadow)     \
    C(MovingWindowTitleStripes)    \
    C(PlaceholderText)             \
    C(Red)                         \
    C(RubberBandBorder)            \
    C(RubberBandFill)              \
    C(Ruler)                       \
    C(RulerActiveText)             \
    C(RulerBorder)                 \
    C(RulerInactiveText)           \
    C(Selection)                   \
    C(SelectionText)               \
    C(SyntaxComment)               \
    C(SyntaxControlKeyword)        \
    C(SyntaxIdentifier)            \
    C(SyntaxKeyword)               \
    C(SyntaxNumber)                \
    C(SyntaxOperator)              \
    C(SyntaxPreprocessorStatement) \
    C(SyntaxPreprocessorValue)     \
    C(SyntaxPunctuation)           \
    C(SyntaxString)                \
    C(SyntaxType)                  \
    C(SyntaxFunction)              \
    C(SyntaxVariable)              \
    C(SyntaxCustomType)            \
    C(SyntaxNamespace)             \
    C(SyntaxMember)                \
    C(SyntaxParameter)             \
    C(TextCursor)                  \
    C(ThreedHighlight)             \
    C(ThreedShadow1)               \
    C(ThreedShadow2)               \
    C(Tooltip)                     \
    C(TooltipText)                 \
    C(Tray)                        \
    C(TrayText)                    \
    C(VisitedLink)                 \
    C(White)                       \
    C(Window)                      \
    C(WindowText)                  \
    C(Yellow)

#define ENUMERATE_FLAG_ROLES(C) \
    C(IsDark)

#define ENUMERATE_PATH_ROLES(C) \
    C(ColorScheme)

enum class ColorRole {
    NoRole,

#undef __ENUMERATE_COLOR_ROLE
#define __ENUMERATE_COLOR_ROLE(role) role,
    ENUMERATE_COLOR_ROLES(__ENUMERATE_COLOR_ROLE)
#undef __ENUMERATE_COLOR_ROLE

        __Count,

    Background = Window,
    DisabledText = ThreedShadow1,
};

enum class FlagRole {
    NoRole,

#undef __ENUMERATE_FLAG_ROLE
#define __ENUMERATE_FLAG_ROLE(role) role,
    ENUMERATE_FLAG_ROLES(__ENUMERATE_FLAG_ROLE)
#undef __ENUMERATE_FLAG_ROLE

        __Count,
};

enum class PathRole {
    NoRole,

#undef __ENUMERATE_PATH_ROLE
#define __ENUMERATE_PATH_ROLE(role) role,
    ENUMERATE_PATH_ROLES(__ENUMERATE_PATH_ROLE)
#undef __ENUMERATE_PATH_ROLE

        __Count,
};

struct SystemTheme {
    BGRA8888 color[(int)ColorRole::__Count];
    bool flag[(int)FlagRole::__Count];
    char path[(int)PathRole::__Count][256]; // TODO: PATH_MAX?
};

void set_system_theme(Core::AnonymousBuffer);
ErrorOr<Core::AnonymousBuffer> load_system_theme(Core::ConfigFile const&, Optional<ByteString> const& color_scheme = OptionalNone());
ErrorOr<Core::AnonymousBuffer> load_system_theme(ByteString const& path, Optional<ByteString> const& color_scheme = OptionalNone());

}

using Gfx::ColorRole;
