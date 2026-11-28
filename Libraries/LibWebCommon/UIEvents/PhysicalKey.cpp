/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebCommon/UIEvents/PhysicalKey.h>

namespace Web::UIEvents {

KeyCode physical_key_from_scan_code(u32 scan_code)
{
    switch (scan_code) {
    case 0x02:
        return Key_1;
    case 0x03:
        return Key_2;
    case 0x04:
        return Key_3;
    case 0x05:
        return Key_4;
    case 0x06:
        return Key_5;
    case 0x07:
        return Key_6;
    case 0x08:
        return Key_7;
    case 0x09:
        return Key_8;
    case 0x0A:
        return Key_9;
    case 0x0B:
        return Key_0;
    case 0x0C:
        return Key_Minus;
    case 0x0D:
        return Key_Equal;
    case 0x10:
        return Key_Q;
    case 0x11:
        return Key_W;
    case 0x12:
        return Key_E;
    case 0x13:
        return Key_R;
    case 0x14:
        return Key_T;
    case 0x15:
        return Key_Y;
    case 0x16:
        return Key_U;
    case 0x17:
        return Key_I;
    case 0x18:
        return Key_O;
    case 0x19:
        return Key_P;
    case 0x1A:
        return Key_LeftBracket;
    case 0x1B:
        return Key_RightBracket;
    case 0x1E:
        return Key_A;
    case 0x1F:
        return Key_S;
    case 0x20:
        return Key_D;
    case 0x21:
        return Key_F;
    case 0x22:
        return Key_G;
    case 0x23:
        return Key_H;
    case 0x24:
        return Key_J;
    case 0x25:
        return Key_K;
    case 0x26:
        return Key_L;
    case 0x27:
        return Key_Semicolon;
    case 0x28:
        return Key_Apostrophe;
    case 0x29:
        return Key_Backtick;
    case 0x2B:
        return Key_Backslash;
    case 0x2C:
        return Key_Z;
    case 0x2D:
        return Key_X;
    case 0x2E:
        return Key_C;
    case 0x2F:
        return Key_V;
    case 0x30:
        return Key_B;
    case 0x31:
        return Key_N;
    case 0x32:
        return Key_M;
    case 0x33:
        return Key_Comma;
    case 0x34:
        return Key_Period;
    case 0x35:
        return Key_Slash;
    default:
        return Key_Invalid;
    }
}

KeyCode physical_key_from_macos_virtual_key(u32 virtual_key)
{
    switch (virtual_key) {
    case 0x00:
        return Key_A;
    case 0x01:
        return Key_S;
    case 0x02:
        return Key_D;
    case 0x03:
        return Key_F;
    case 0x04:
        return Key_H;
    case 0x05:
        return Key_G;
    case 0x06:
        return Key_Z;
    case 0x07:
        return Key_X;
    case 0x08:
        return Key_C;
    case 0x09:
        return Key_V;
    // NB: An ISO keyboard reports its key left of 1 as kVK_ISO_Section. kVK_ANSI_Grave is that key on an ANSI keyboard
    //     but the key right of the left Shift on an ISO one, so it's left to the key the layout reports.
    case 0x0A:
        return Key_Backtick;
    case 0x0B:
        return Key_B;
    case 0x0C:
        return Key_Q;
    case 0x0D:
        return Key_W;
    case 0x0E:
        return Key_E;
    case 0x0F:
        return Key_R;
    case 0x10:
        return Key_Y;
    case 0x11:
        return Key_T;
    case 0x12:
        return Key_1;
    case 0x13:
        return Key_2;
    case 0x14:
        return Key_3;
    case 0x15:
        return Key_4;
    case 0x16:
        return Key_6;
    case 0x17:
        return Key_5;
    case 0x18:
        return Key_Equal;
    case 0x19:
        return Key_9;
    case 0x1A:
        return Key_7;
    case 0x1B:
        return Key_Minus;
    case 0x1C:
        return Key_8;
    case 0x1D:
        return Key_0;
    case 0x1E:
        return Key_RightBracket;
    case 0x1F:
        return Key_O;
    case 0x20:
        return Key_U;
    case 0x21:
        return Key_LeftBracket;
    case 0x22:
        return Key_I;
    case 0x23:
        return Key_P;
    case 0x25:
        return Key_L;
    case 0x26:
        return Key_J;
    case 0x27:
        return Key_Apostrophe;
    case 0x28:
        return Key_K;
    case 0x29:
        return Key_Semicolon;
    case 0x2A:
        return Key_Backslash;
    case 0x2B:
        return Key_Comma;
    case 0x2C:
        return Key_Slash;
    case 0x2D:
        return Key_N;
    case 0x2E:
        return Key_M;
    case 0x2F:
        return Key_Period;
    default:
        return Key_Invalid;
    }
}

}
