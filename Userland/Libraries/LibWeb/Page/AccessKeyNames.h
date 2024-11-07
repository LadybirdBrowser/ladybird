/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>

namespace Web::HTML::AccessKeyNames {

#define ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)                                                             \
    __ENUMERATE_ACCESS_KEY(AccessKey_0, '0', "Alt+Shift+0", "⌃⌥0", KeyCode::Key_0, KeyCode::Key_RightParen)       \
    __ENUMERATE_ACCESS_KEY(AccessKey_1, '1', "Alt+Shift+1", "⌃⌥1", KeyCode::Key_1, KeyCode::Key_ExclamationPoint) \
    __ENUMERATE_ACCESS_KEY(AccessKey_2, '2', "Alt+Shift+2", "⌃⌥2", KeyCode::Key_2, KeyCode::Key_AtSign)           \
    __ENUMERATE_ACCESS_KEY(AccessKey_3, '3', "Alt+Shift+3", "⌃⌥3", KeyCode::Key_3, KeyCode::Key_Hashtag)          \
    __ENUMERATE_ACCESS_KEY(AccessKey_4, '4', "Alt+Shift+4", "⌃⌥4", KeyCode::Key_4, KeyCode::Key_Dollar)           \
    __ENUMERATE_ACCESS_KEY(AccessKey_5, '5', "Alt+Shift+5", "⌃⌥5", KeyCode::Key_5, KeyCode::Key_Percent)          \
    __ENUMERATE_ACCESS_KEY(AccessKey_6, '6', "Alt+Shift+6", "⌃⌥6", KeyCode::Key_6, KeyCode::Key_Circumflex)       \
    __ENUMERATE_ACCESS_KEY(AccessKey_7, '7', "Alt+Shift+7", "⌃⌥7", KeyCode::Key_7, KeyCode::Key_Ampersand)        \
    __ENUMERATE_ACCESS_KEY(AccessKey_8, '8', "Alt+Shift+8", "⌃⌥8", KeyCode::Key_8, KeyCode::Key_Asterisk)         \
    __ENUMERATE_ACCESS_KEY(AccessKey_9, '9', "Alt+Shift+9", "⌃⌥9", KeyCode::Key_9, KeyCode::Key_LeftParen)        \
    __ENUMERATE_ACCESS_KEY(AccessKey_A, 'A', "Alt+Shift+A", "⌃⌥A", KeyCode::Key_A, KeyCode::Key_A)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_B, 'B', "Alt+Shift+B", "⌃⌥B", KeyCode::Key_B, KeyCode::Key_B)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_C, 'C', "Alt+Shift+C", "⌃⌥C", KeyCode::Key_C, KeyCode::Key_C)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_D, 'D', "Alt+Shift+D", "⌃⌥D", KeyCode::Key_D, KeyCode::Key_D)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_E, 'E', "Alt+Shift+E", "⌃⌥E", KeyCode::Key_E, KeyCode::Key_E)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_F, 'F', "Alt+Shift+F", "⌃⌥F", KeyCode::Key_F, KeyCode::Key_F)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_G, 'G', "Alt+Shift+G", "⌃⌥G", KeyCode::Key_G, KeyCode::Key_G)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_H, 'H', "Alt+Shift+H", "⌃⌥H", KeyCode::Key_H, KeyCode::Key_H)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_I, 'I', "Alt+Shift+I", "⌃⌥I", KeyCode::Key_I, KeyCode::Key_I)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_J, 'J', "Alt+Shift+J", "⌃⌥J", KeyCode::Key_J, KeyCode::Key_J)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_K, 'K', "Alt+Shift+K", "⌃⌥K", KeyCode::Key_K, KeyCode::Key_K)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_L, 'L', "Alt+Shift+L", "⌃⌥L", KeyCode::Key_L, KeyCode::Key_L)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_M, 'M', "Alt+Shift+M", "⌃⌥M", KeyCode::Key_M, KeyCode::Key_M)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_N, 'N', "Alt+Shift+N", "⌃⌥N", KeyCode::Key_N, KeyCode::Key_N)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_O, 'O', "Alt+Shift+O", "⌃⌥O", KeyCode::Key_O, KeyCode::Key_O)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_P, 'P', "Alt+Shift+P", "⌃⌥P", KeyCode::Key_P, KeyCode::Key_P)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_Q, 'Q', "Alt+Shift+Q", "⌃⌥Q", KeyCode::Key_Q, KeyCode::Key_Q)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_R, 'R', "Alt+Shift+R", "⌃⌥R", KeyCode::Key_R, KeyCode::Key_R)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_S, 'S', "Alt+Shift+S", "⌃⌥S", KeyCode::Key_S, KeyCode::Key_S)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_T, 'T', "Alt+Shift+T", "⌃⌥T", KeyCode::Key_T, KeyCode::Key_T)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_U, 'U', "Alt+Shift+U", "⌃⌥U", KeyCode::Key_U, KeyCode::Key_U)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_V, 'V', "Alt+Shift+V", "⌃⌥V", KeyCode::Key_V, KeyCode::Key_V)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_W, 'W', "Alt+Shift+W", "⌃⌥W", KeyCode::Key_W, KeyCode::Key_W)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_X, 'X', "Alt+Shift+X", "⌃⌥X", KeyCode::Key_X, KeyCode::Key_X)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_Y, 'Y', "Alt+Shift+Y", "⌃⌥Y", KeyCode::Key_Y, KeyCode::Key_Y)                \
    __ENUMERATE_ACCESS_KEY(AccessKey_Z, 'Z', "Alt+Shift+Z", "⌃⌥Z", KeyCode::Key_Z, KeyCode::Key_Z)

#define __ENUMERATE_ACCESS_KEY(name, character, label, maclabel, code, shiftcode) extern FlyString name /* NOLINT(misc-confusable-identifiers) */;
ENUMERATE_ACCESS_KEYS(__ENUMERATE_ACCESS_KEY)
#undef __ENUMERATE_ACCESS_KEY

void initialize_strings();

}
