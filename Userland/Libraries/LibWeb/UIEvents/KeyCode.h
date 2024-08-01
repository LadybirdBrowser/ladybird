/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Platform.h>
#include <AK/Types.h>

namespace Web::UIEvents {

#define ENUMERATE_KEY_CODES                                    \
    __ENUMERATE_KEY_CODE(Invalid, "Invalid")                   \
    __ENUMERATE_KEY_CODE(Escape, "Escape")                     \
    __ENUMERATE_KEY_CODE(Tab, "Tab")                           \
    __ENUMERATE_KEY_CODE(Backspace, "Backspace")               \
    __ENUMERATE_KEY_CODE(Return, "Return")                     \
    __ENUMERATE_KEY_CODE(Insert, "Insert")                     \
    __ENUMERATE_KEY_CODE(Delete, "Delete")                     \
    __ENUMERATE_KEY_CODE(PrintScreen, "PrintScreen")           \
    __ENUMERATE_KEY_CODE(PauseBreak, "PauseBreak")             \
    __ENUMERATE_KEY_CODE(SysRq, "SysRq")                       \
    __ENUMERATE_KEY_CODE(Home, "Home")                         \
    __ENUMERATE_KEY_CODE(End, "End")                           \
    __ENUMERATE_KEY_CODE(Left, "Left")                         \
    __ENUMERATE_KEY_CODE(Up, "Up")                             \
    __ENUMERATE_KEY_CODE(Right, "Right")                       \
    __ENUMERATE_KEY_CODE(Down, "Down")                         \
    __ENUMERATE_KEY_CODE(PageUp, "PageUp")                     \
    __ENUMERATE_KEY_CODE(PageDown, "PageDown")                 \
    __ENUMERATE_KEY_CODE(LeftShift, "LeftShift")               \
    __ENUMERATE_KEY_CODE(RightShift, "RightShift")             \
    __ENUMERATE_KEY_CODE(Control, "Ctrl")                      \
    __ENUMERATE_KEY_CODE(RightControl, "RightCtrl")            \
    __ENUMERATE_KEY_CODE(Alt, "Alt")                           \
    __ENUMERATE_KEY_CODE(RightAlt, "Alt")                      \
    __ENUMERATE_KEY_CODE(CapsLock, "CapsLock")                 \
    __ENUMERATE_KEY_CODE(NumLock, "NumLock")                   \
    __ENUMERATE_KEY_CODE(ScrollLock, "ScrollLock")             \
    __ENUMERATE_KEY_CODE(F1, "F1")                             \
    __ENUMERATE_KEY_CODE(F2, "F2")                             \
    __ENUMERATE_KEY_CODE(F3, "F3")                             \
    __ENUMERATE_KEY_CODE(F4, "F4")                             \
    __ENUMERATE_KEY_CODE(F5, "F5")                             \
    __ENUMERATE_KEY_CODE(F6, "F6")                             \
    __ENUMERATE_KEY_CODE(F7, "F7")                             \
    __ENUMERATE_KEY_CODE(F8, "F8")                             \
    __ENUMERATE_KEY_CODE(F9, "F9")                             \
    __ENUMERATE_KEY_CODE(F10, "F10")                           \
    __ENUMERATE_KEY_CODE(F11, "F11")                           \
    __ENUMERATE_KEY_CODE(F12, "F12")                           \
    __ENUMERATE_KEY_CODE(Space, "Space")                       \
    __ENUMERATE_KEY_CODE(ExclamationPoint, "!")                \
    __ENUMERATE_KEY_CODE(DoubleQuote, "\"")                    \
    __ENUMERATE_KEY_CODE(Hashtag, "#")                         \
    __ENUMERATE_KEY_CODE(Dollar, "$")                          \
    __ENUMERATE_KEY_CODE(Percent, "%")                         \
    __ENUMERATE_KEY_CODE(Ampersand, "&")                       \
    __ENUMERATE_KEY_CODE(Apostrophe, "'")                      \
    __ENUMERATE_KEY_CODE(LeftParen, "(")                       \
    __ENUMERATE_KEY_CODE(RightParen, ")")                      \
    __ENUMERATE_KEY_CODE(Asterisk, "*")                        \
    __ENUMERATE_KEY_CODE(Plus, "+")                            \
    __ENUMERATE_KEY_CODE(Comma, ",")                           \
    __ENUMERATE_KEY_CODE(Minus, "-")                           \
    __ENUMERATE_KEY_CODE(Period, ".")                          \
    __ENUMERATE_KEY_CODE(Slash, "/")                           \
    __ENUMERATE_KEY_CODE(0, "0")                               \
    __ENUMERATE_KEY_CODE(1, "1")                               \
    __ENUMERATE_KEY_CODE(2, "2")                               \
    __ENUMERATE_KEY_CODE(3, "3")                               \
    __ENUMERATE_KEY_CODE(4, "4")                               \
    __ENUMERATE_KEY_CODE(5, "5")                               \
    __ENUMERATE_KEY_CODE(6, "6")                               \
    __ENUMERATE_KEY_CODE(7, "7")                               \
    __ENUMERATE_KEY_CODE(8, "8")                               \
    __ENUMERATE_KEY_CODE(9, "9")                               \
    __ENUMERATE_KEY_CODE(Colon, ":")                           \
    __ENUMERATE_KEY_CODE(Semicolon, ";")                       \
    __ENUMERATE_KEY_CODE(LessThan, "<")                        \
    __ENUMERATE_KEY_CODE(Equal, "=")                           \
    __ENUMERATE_KEY_CODE(GreaterThan, ">")                     \
    __ENUMERATE_KEY_CODE(QuestionMark, "?")                    \
    __ENUMERATE_KEY_CODE(AtSign, "@")                          \
    __ENUMERATE_KEY_CODE(A, "A")                               \
    __ENUMERATE_KEY_CODE(B, "B")                               \
    __ENUMERATE_KEY_CODE(C, "C")                               \
    __ENUMERATE_KEY_CODE(D, "D")                               \
    __ENUMERATE_KEY_CODE(E, "E")                               \
    __ENUMERATE_KEY_CODE(F, "F")                               \
    __ENUMERATE_KEY_CODE(G, "G")                               \
    __ENUMERATE_KEY_CODE(H, "H")                               \
    __ENUMERATE_KEY_CODE(I, "I")                               \
    __ENUMERATE_KEY_CODE(J, "J")                               \
    __ENUMERATE_KEY_CODE(K, "K")                               \
    __ENUMERATE_KEY_CODE(L, "L")                               \
    __ENUMERATE_KEY_CODE(M, "M")                               \
    __ENUMERATE_KEY_CODE(N, "N")                               \
    __ENUMERATE_KEY_CODE(O, "O")                               \
    __ENUMERATE_KEY_CODE(P, "P")                               \
    __ENUMERATE_KEY_CODE(Q, "Q")                               \
    __ENUMERATE_KEY_CODE(R, "R")                               \
    __ENUMERATE_KEY_CODE(S, "S")                               \
    __ENUMERATE_KEY_CODE(T, "T")                               \
    __ENUMERATE_KEY_CODE(U, "U")                               \
    __ENUMERATE_KEY_CODE(V, "V")                               \
    __ENUMERATE_KEY_CODE(W, "W")                               \
    __ENUMERATE_KEY_CODE(X, "X")                               \
    __ENUMERATE_KEY_CODE(Y, "Y")                               \
    __ENUMERATE_KEY_CODE(Z, "Z")                               \
    __ENUMERATE_KEY_CODE(LeftBracket, "[")                     \
    __ENUMERATE_KEY_CODE(RightBracket, "]")                    \
    __ENUMERATE_KEY_CODE(Backslash, "\\")                      \
    __ENUMERATE_KEY_CODE(Circumflex, "^")                      \
    __ENUMERATE_KEY_CODE(Underscore, "_")                      \
    __ENUMERATE_KEY_CODE(LeftBrace, "{")                       \
    __ENUMERATE_KEY_CODE(RightBrace, "}")                      \
    __ENUMERATE_KEY_CODE(Pipe, "|")                            \
    __ENUMERATE_KEY_CODE(Tilde, "~")                           \
    __ENUMERATE_KEY_CODE(Backtick, "`")                        \
    __ENUMERATE_KEY_CODE(Super, "Super")                       \
    __ENUMERATE_KEY_CODE(BrowserSearch, "BrowserSearch")       \
    __ENUMERATE_KEY_CODE(BrowserFavorites, "BrowserFavorites") \
    __ENUMERATE_KEY_CODE(BrowserHome, "BrowserHome")           \
    __ENUMERATE_KEY_CODE(PreviousTrack, "PreviousTrack")       \
    __ENUMERATE_KEY_CODE(BrowserBack, "BrowserBack")           \
    __ENUMERATE_KEY_CODE(BrowserForward, "BrowserForward")     \
    __ENUMERATE_KEY_CODE(BrowserRefresh, "BrowserRefresh")     \
    __ENUMERATE_KEY_CODE(BrowserStop, "BrowserStop")           \
    __ENUMERATE_KEY_CODE(VolumeDown, "VolumeDown")             \
    __ENUMERATE_KEY_CODE(VolumeUp, "VolumeUp")                 \
    __ENUMERATE_KEY_CODE(Wake, "Wake")                         \
    __ENUMERATE_KEY_CODE(Sleep, "Sleep")                       \
    __ENUMERATE_KEY_CODE(NextTrack, "NextTrack")               \
    __ENUMERATE_KEY_CODE(MediaSelect, "MediaSelect")           \
    __ENUMERATE_KEY_CODE(Email, "Email")                       \
    __ENUMERATE_KEY_CODE(MyComputer, "MyComputer")             \
    __ENUMERATE_KEY_CODE(Power, "Power")                       \
    __ENUMERATE_KEY_CODE(Stop, "Stop")                         \
    __ENUMERATE_KEY_CODE(LeftGUI, "LeftGUI")                   \
    __ENUMERATE_KEY_CODE(Mute, "Mute")                         \
    __ENUMERATE_KEY_CODE(RightGUI, "RightGUI")                 \
    __ENUMERATE_KEY_CODE(Calculator, "Calculator")             \
    __ENUMERATE_KEY_CODE(Apps, "Apps")                         \
    __ENUMERATE_KEY_CODE(PlayPause, "PlayPause")               \
    __ENUMERATE_KEY_CODE(Menu, "Menu")

enum KeyCode : u8 {
#define __ENUMERATE_KEY_CODE(name, ui_name) Key_##name,
    ENUMERATE_KEY_CODES
#undef __ENUMERATE_KEY_CODE

        Key_Shift
    = Key_LeftShift,
};
size_t const key_code_count = Key_Menu + 1;

enum KeyModifier {
    Mod_None = 0x00,
    Mod_Alt = (1 << 0),
    Mod_Ctrl = (1 << 1),
    Mod_Shift = (1 << 2),
    Mod_Super = (1 << 3),
    Mod_AltGr = (1 << 4),
    Mod_Keypad = (1 << 5),
    Mod_Mask = Mod_Alt | Mod_Ctrl | Mod_Shift | Mod_Super | Mod_AltGr | Mod_Keypad,

    Is_Press = 0x80,

#if defined(AK_OS_MACOS)
    Mod_PlatformCtrl = Mod_Super,
#else
    Mod_PlatformCtrl = Mod_Ctrl,
#endif
};

AK_ENUM_BITWISE_OPERATORS(KeyModifier);

inline KeyCode code_point_to_key_code(u32 code_point)
{
    switch (code_point) {
#define MATCH_ALPHA(letter) \
    case #letter[0]:        \
    case #letter[0] + 32:   \
        return KeyCode::Key_##letter;
        MATCH_ALPHA(A)
        MATCH_ALPHA(B)
        MATCH_ALPHA(C)
        MATCH_ALPHA(D)
        MATCH_ALPHA(E)
        MATCH_ALPHA(F)
        MATCH_ALPHA(G)
        MATCH_ALPHA(H)
        MATCH_ALPHA(I)
        MATCH_ALPHA(J)
        MATCH_ALPHA(K)
        MATCH_ALPHA(L)
        MATCH_ALPHA(M)
        MATCH_ALPHA(N)
        MATCH_ALPHA(O)
        MATCH_ALPHA(P)
        MATCH_ALPHA(Q)
        MATCH_ALPHA(R)
        MATCH_ALPHA(S)
        MATCH_ALPHA(T)
        MATCH_ALPHA(U)
        MATCH_ALPHA(V)
        MATCH_ALPHA(W)
        MATCH_ALPHA(X)
        MATCH_ALPHA(Y)
        MATCH_ALPHA(Z)
#undef MATCH_ALPHA

#define MATCH_KEY(name, character) \
    case character:                \
        return KeyCode::Key_##name;
        MATCH_KEY(ExclamationPoint, '!')
        MATCH_KEY(DoubleQuote, '"')
        MATCH_KEY(Hashtag, '#')
        MATCH_KEY(Dollar, '$')
        MATCH_KEY(Percent, '%')
        MATCH_KEY(Ampersand, '&')
        MATCH_KEY(Apostrophe, '\'')
        MATCH_KEY(LeftParen, '(')
        MATCH_KEY(RightParen, ')')
        MATCH_KEY(Asterisk, '*')
        MATCH_KEY(Plus, '+')
        MATCH_KEY(Comma, ',')
        MATCH_KEY(Minus, '-')
        MATCH_KEY(Period, '.')
        MATCH_KEY(Slash, '/')
        MATCH_KEY(0, '0')
        MATCH_KEY(1, '1')
        MATCH_KEY(2, '2')
        MATCH_KEY(3, '3')
        MATCH_KEY(4, '4')
        MATCH_KEY(5, '5')
        MATCH_KEY(6, '6')
        MATCH_KEY(7, '7')
        MATCH_KEY(8, '8')
        MATCH_KEY(9, '9')
        MATCH_KEY(Colon, ':')
        MATCH_KEY(Semicolon, ';')
        MATCH_KEY(LessThan, '<')
        MATCH_KEY(Equal, '=')
        MATCH_KEY(GreaterThan, '>')
        MATCH_KEY(QuestionMark, '?')
        MATCH_KEY(AtSign, '@')
        MATCH_KEY(LeftBracket, '[')
        MATCH_KEY(RightBracket, ']')
        MATCH_KEY(Backslash, '\\')
        MATCH_KEY(Circumflex, '^')
        MATCH_KEY(Underscore, '_')
        MATCH_KEY(LeftBrace, '{')
        MATCH_KEY(RightBrace, '}')
        MATCH_KEY(Pipe, '|')
        MATCH_KEY(Tilde, '~')
        MATCH_KEY(Backtick, '`')
        MATCH_KEY(Space, ' ')
        MATCH_KEY(Tab, '\t')
        MATCH_KEY(Backspace, '\b')
#undef MATCH_KEY

    default:
        return KeyCode::Key_Invalid;
    }
}

}
