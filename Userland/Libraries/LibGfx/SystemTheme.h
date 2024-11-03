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
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ConfigFile.h>
#include <LibGfx/Color.h>
#include <LibGfx/TextAlignment.h>

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

#define ENUMERATE_ALIGNMENT_ROLES(C) \
    C(TitleAlignment)

#define ENUMERATE_FLAG_ROLES(C) \
    C(BoldTextAsBright)         \
    C(IsDark)                   \
    C(TitleButtonsIconOnly)

#define ENUMERATE_METRIC_ROLES(C) \
    C(BorderThickness)            \
    C(BorderRadius)               \
    C(TitleHeight)                \
    C(TitleButtonWidth)           \
    C(TitleButtonHeight)

#define ENUMERATE_PATH_ROLES(C) \
    C(TitleButtonIcons)         \
    C(InactiveWindowShadow)     \
    C(ActiveWindowShadow)       \
    C(TaskbarShadow)            \
    C(MenuShadow)               \
    C(TooltipShadow)            \
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

inline StringView to_string(ColorRole role)
{
    switch (role) {
    case ColorRole::NoRole:
        return "NoRole"sv;
#undef __ENUMERATE_COLOR_ROLE
#define __ENUMERATE_COLOR_ROLE(role) \
    case ColorRole::role:            \
        return #role##sv;
        ENUMERATE_COLOR_ROLES(__ENUMERATE_COLOR_ROLE)
#undef __ENUMERATE_COLOR_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

enum class AlignmentRole {
    NoRole,

#undef __ENUMERATE_ALIGNMENT_ROLE
#define __ENUMERATE_ALIGNMENT_ROLE(role) role,
    ENUMERATE_ALIGNMENT_ROLES(__ENUMERATE_ALIGNMENT_ROLE)
#undef __ENUMERATE_ALIGNMENT_ROLE

        __Count,
};

inline StringView to_string(AlignmentRole role)
{
    switch (role) {
    case AlignmentRole::NoRole:
        return "NoRole"sv;
#undef __ENUMERATE_ALIGNMENT_ROLE
#define __ENUMERATE_ALIGNMENT_ROLE(role) \
    case AlignmentRole::role:            \
        return #role##sv;
        ENUMERATE_ALIGNMENT_ROLES(__ENUMERATE_ALIGNMENT_ROLE)
#undef __ENUMERATE_ALIGNMENT_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

enum class FlagRole {
    NoRole,

#undef __ENUMERATE_FLAG_ROLE
#define __ENUMERATE_FLAG_ROLE(role) role,
    ENUMERATE_FLAG_ROLES(__ENUMERATE_FLAG_ROLE)
#undef __ENUMERATE_FLAG_ROLE

        __Count,
};

inline StringView to_string(FlagRole role)
{
    switch (role) {
    case FlagRole::NoRole:
        return "NoRole"sv;
#undef __ENUMERATE_FLAG_ROLE
#define __ENUMERATE_FLAG_ROLE(role) \
    case FlagRole::role:            \
        return #role##sv;
        ENUMERATE_FLAG_ROLES(__ENUMERATE_FLAG_ROLE)
#undef __ENUMERATE_FLAG_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

enum class MetricRole {
    NoRole,

#undef __ENUMERATE_METRIC_ROLE
#define __ENUMERATE_METRIC_ROLE(role) role,
    ENUMERATE_METRIC_ROLES(__ENUMERATE_METRIC_ROLE)
#undef __ENUMERATE_METRIC_ROLE

        __Count,
};

inline StringView to_string(MetricRole role)
{
    switch (role) {
    case MetricRole::NoRole:
        return "NoRole"sv;
#undef __ENUMERATE_METRIC_ROLE
#define __ENUMERATE_METRIC_ROLE(role) \
    case MetricRole::role:            \
        return #role##sv;
        ENUMERATE_METRIC_ROLES(__ENUMERATE_METRIC_ROLE)
#undef __ENUMERATE_METRIC_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

enum class PathRole {
    NoRole,

#undef __ENUMERATE_PATH_ROLE
#define __ENUMERATE_PATH_ROLE(role) role,
    ENUMERATE_PATH_ROLES(__ENUMERATE_PATH_ROLE)
#undef __ENUMERATE_PATH_ROLE

        __Count,
};

inline StringView to_string(PathRole role)
{
    switch (role) {
    case PathRole::NoRole:
        return "NoRole"sv;
#undef __ENUMERATE_PATH_ROLE
#define __ENUMERATE_PATH_ROLE(role) \
    case PathRole::role:            \
        return #role##sv;
        ENUMERATE_PATH_ROLES(__ENUMERATE_PATH_ROLE)
#undef __ENUMERATE_PATH_ROLE
    default:
        VERIFY_NOT_REACHED();
    }
}

struct SystemTheme {
    ARGB32 color[(int)ColorRole::__Count];
    Gfx::TextAlignment alignment[(int)AlignmentRole::__Count];
    bool flag[(int)FlagRole::__Count];
    int metric[(int)MetricRole::__Count];
    char path[(int)PathRole::__Count][256]; // TODO: PATH_MAX?
};

Core::AnonymousBuffer& current_system_theme_buffer();
void set_system_theme(Core::AnonymousBuffer);
ErrorOr<Core::AnonymousBuffer> load_system_theme(Core::ConfigFile const&, Optional<ByteString> const& color_scheme = OptionalNone());
ErrorOr<Core::AnonymousBuffer> load_system_theme(ByteString const& path, Optional<ByteString> const& color_scheme = OptionalNone());

struct SystemThemeMetaData {
    ByteString name;
    ByteString menu_name;
    ByteString path;
};

ErrorOr<Vector<SystemThemeMetaData>> list_installed_system_themes();

}

using Gfx::ColorRole;
