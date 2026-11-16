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
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ConfigFile.h>
#include <LibGfx/Color.h>

namespace Gfx {

#define ENUMERATE_COLOR_ROLES(C)   \
    C(Accent)                      \
    C(Base)                        \
    C(BaseText)                    \
    C(Black)                       \
    C(Blue)                        \
    C(Button)                      \
    C(ButtonText)                  \
    C(ColorSchemeBackground)       \
    C(ColorSchemeForeground)       \
    C(Cyan)                        \
    C(Green)                       \
    C(Gutter)                      \
    C(InactiveSelection)           \
    C(LightSelection)              \
    C(DarkSelection)               \
    C(Link)                        \
    C(Magenta)                     \
    C(PlaceholderText)             \
    C(Red)                         \
    C(Selection)                   \
    C(SyntaxComment)               \
    C(SyntaxControlKeyword)        \
    C(SyntaxIdentifier)            \
    C(SyntaxKeyword)               \
    C(SyntaxNumber)                \
    C(SyntaxOperator)              \
    C(SyntaxPreprocessorStatement) \
    C(SyntaxPunctuation)           \
    C(SyntaxString)                \
    C(SyntaxType)                  \
    C(ThreedShadow1)               \
    C(Tooltip)                     \
    C(TooltipText)                 \
    C(White)                       \
    C(Window)                      \
    C(Yellow)

#define ENUMERATE_FLAG_ROLES(C) \
    C(IsDark)

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

struct SystemTheme {
    BGRA8888 color[(int)ColorRole::__Count];
    bool flag[(int)FlagRole::__Count];
};

ErrorOr<Core::AnonymousBuffer> load_system_theme(Core::ConfigFile const&);
ErrorOr<Core::AnonymousBuffer> load_system_theme(ByteString const& path);

}

using Gfx::ColorRole;
