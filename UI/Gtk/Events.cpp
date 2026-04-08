/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Gtk/Events.h>

namespace Ladybird {

// GDK doesn't define constants for extended mouse buttons
static constexpr guint GDK_BUTTON_BACKWARD = 8;
static constexpr guint GDK_BUTTON_FORWARD = 9;

Web::UIEvents::MouseButton gdk_buttons_to_web(GdkModifierType state)
{
    unsigned buttons = Web::UIEvents::MouseButton::None;
    if (state & GDK_BUTTON1_MASK)
        buttons |= Web::UIEvents::MouseButton::Primary;
    if (state & GDK_BUTTON2_MASK)
        buttons |= Web::UIEvents::MouseButton::Middle;
    if (state & GDK_BUTTON3_MASK)
        buttons |= Web::UIEvents::MouseButton::Secondary;
    if (state & GDK_BUTTON4_MASK)
        buttons |= Web::UIEvents::MouseButton::Backward;
    if (state & GDK_BUTTON5_MASK)
        buttons |= Web::UIEvents::MouseButton::Forward;
    return static_cast<Web::UIEvents::MouseButton>(buttons);
}

Web::UIEvents::MouseButton gdk_button_to_web(guint button)
{
    switch (button) {
    case GDK_BUTTON_PRIMARY:
        return Web::UIEvents::MouseButton::Primary;
    case GDK_BUTTON_MIDDLE:
        return Web::UIEvents::MouseButton::Middle;
    case GDK_BUTTON_SECONDARY:
        return Web::UIEvents::MouseButton::Secondary;
    case GDK_BUTTON_BACKWARD:
        return Web::UIEvents::MouseButton::Backward;
    case GDK_BUTTON_FORWARD:
        return Web::UIEvents::MouseButton::Forward;
    default:
        return Web::UIEvents::MouseButton::Primary;
    }
}

Web::UIEvents::KeyModifier gdk_modifier_to_web(GdkModifierType state)
{
    unsigned modifiers = Web::UIEvents::KeyModifier::Mod_None;
    if (state & GDK_SHIFT_MASK)
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    if (state & GDK_CONTROL_MASK)
        modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
    if (state & GDK_ALT_MASK)
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    if (state & GDK_SUPER_MASK)
        modifiers |= Web::UIEvents::KeyModifier::Mod_Super;
    return static_cast<Web::UIEvents::KeyModifier>(modifiers);
}

Web::UIEvents::KeyCode gdk_keyval_to_web(guint keyval)
{
    switch (keyval) {
    case GDK_KEY_BackSpace:
        return Web::UIEvents::KeyCode::Key_Backspace;
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
        return Web::UIEvents::KeyCode::Key_Tab;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        return Web::UIEvents::KeyCode::Key_Return;
    case GDK_KEY_Escape:
        return Web::UIEvents::KeyCode::Key_Escape;
    case GDK_KEY_space:
        return Web::UIEvents::KeyCode::Key_Space;
    case GDK_KEY_Page_Up:
        return Web::UIEvents::KeyCode::Key_PageUp;
    case GDK_KEY_Page_Down:
        return Web::UIEvents::KeyCode::Key_PageDown;
    case GDK_KEY_End:
        return Web::UIEvents::KeyCode::Key_End;
    case GDK_KEY_Home:
        return Web::UIEvents::KeyCode::Key_Home;
    case GDK_KEY_Left:
        return Web::UIEvents::KeyCode::Key_Left;
    case GDK_KEY_Up:
        return Web::UIEvents::KeyCode::Key_Up;
    case GDK_KEY_Right:
        return Web::UIEvents::KeyCode::Key_Right;
    case GDK_KEY_Down:
        return Web::UIEvents::KeyCode::Key_Down;
    case GDK_KEY_Delete:
        return Web::UIEvents::KeyCode::Key_Delete;
    case GDK_KEY_Insert:
        return Web::UIEvents::KeyCode::Key_Insert;
    case GDK_KEY_Shift_L:
        return Web::UIEvents::KeyCode::Key_LeftShift;
    case GDK_KEY_Shift_R:
        return Web::UIEvents::KeyCode::Key_RightShift;
    case GDK_KEY_Control_L:
        return Web::UIEvents::KeyCode::Key_LeftControl;
    case GDK_KEY_Control_R:
        return Web::UIEvents::KeyCode::Key_RightControl;
    case GDK_KEY_Alt_L:
        return Web::UIEvents::KeyCode::Key_LeftAlt;
    case GDK_KEY_Alt_R:
        return Web::UIEvents::KeyCode::Key_RightAlt;
    case GDK_KEY_Super_L:
        return Web::UIEvents::KeyCode::Key_LeftSuper;
    case GDK_KEY_Super_R:
        return Web::UIEvents::KeyCode::Key_RightSuper;
    case GDK_KEY_Caps_Lock:
        return Web::UIEvents::KeyCode::Key_CapsLock;
    case GDK_KEY_Num_Lock:
        return Web::UIEvents::KeyCode::Key_NumLock;
    case GDK_KEY_Scroll_Lock:
        return Web::UIEvents::KeyCode::Key_ScrollLock;
    case GDK_KEY_Print:
        return Web::UIEvents::KeyCode::Key_PrintScreen;
    case GDK_KEY_F1:
        return Web::UIEvents::KeyCode::Key_F1;
    case GDK_KEY_F2:
        return Web::UIEvents::KeyCode::Key_F2;
    case GDK_KEY_F3:
        return Web::UIEvents::KeyCode::Key_F3;
    case GDK_KEY_F4:
        return Web::UIEvents::KeyCode::Key_F4;
    case GDK_KEY_F5:
        return Web::UIEvents::KeyCode::Key_F5;
    case GDK_KEY_F6:
        return Web::UIEvents::KeyCode::Key_F6;
    case GDK_KEY_F7:
        return Web::UIEvents::KeyCode::Key_F7;
    case GDK_KEY_F8:
        return Web::UIEvents::KeyCode::Key_F8;
    case GDK_KEY_F9:
        return Web::UIEvents::KeyCode::Key_F9;
    case GDK_KEY_F10:
        return Web::UIEvents::KeyCode::Key_F10;
    case GDK_KEY_F11:
        return Web::UIEvents::KeyCode::Key_F11;
    case GDK_KEY_F12:
        return Web::UIEvents::KeyCode::Key_F12;
    case GDK_KEY_a:
    case GDK_KEY_A:
        return Web::UIEvents::KeyCode::Key_A;
    case GDK_KEY_b:
    case GDK_KEY_B:
        return Web::UIEvents::KeyCode::Key_B;
    case GDK_KEY_c:
    case GDK_KEY_C:
        return Web::UIEvents::KeyCode::Key_C;
    case GDK_KEY_d:
    case GDK_KEY_D:
        return Web::UIEvents::KeyCode::Key_D;
    case GDK_KEY_e:
    case GDK_KEY_E:
        return Web::UIEvents::KeyCode::Key_E;
    case GDK_KEY_f:
    case GDK_KEY_F:
        return Web::UIEvents::KeyCode::Key_F;
    case GDK_KEY_g:
    case GDK_KEY_G:
        return Web::UIEvents::KeyCode::Key_G;
    case GDK_KEY_h:
    case GDK_KEY_H:
        return Web::UIEvents::KeyCode::Key_H;
    case GDK_KEY_i:
    case GDK_KEY_I:
        return Web::UIEvents::KeyCode::Key_I;
    case GDK_KEY_j:
    case GDK_KEY_J:
        return Web::UIEvents::KeyCode::Key_J;
    case GDK_KEY_k:
    case GDK_KEY_K:
        return Web::UIEvents::KeyCode::Key_K;
    case GDK_KEY_l:
    case GDK_KEY_L:
        return Web::UIEvents::KeyCode::Key_L;
    case GDK_KEY_m:
    case GDK_KEY_M:
        return Web::UIEvents::KeyCode::Key_M;
    case GDK_KEY_n:
    case GDK_KEY_N:
        return Web::UIEvents::KeyCode::Key_N;
    case GDK_KEY_o:
    case GDK_KEY_O:
        return Web::UIEvents::KeyCode::Key_O;
    case GDK_KEY_p:
    case GDK_KEY_P:
        return Web::UIEvents::KeyCode::Key_P;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        return Web::UIEvents::KeyCode::Key_Q;
    case GDK_KEY_r:
    case GDK_KEY_R:
        return Web::UIEvents::KeyCode::Key_R;
    case GDK_KEY_s:
    case GDK_KEY_S:
        return Web::UIEvents::KeyCode::Key_S;
    case GDK_KEY_t:
    case GDK_KEY_T:
        return Web::UIEvents::KeyCode::Key_T;
    case GDK_KEY_u:
    case GDK_KEY_U:
        return Web::UIEvents::KeyCode::Key_U;
    case GDK_KEY_v:
    case GDK_KEY_V:
        return Web::UIEvents::KeyCode::Key_V;
    case GDK_KEY_w:
    case GDK_KEY_W:
        return Web::UIEvents::KeyCode::Key_W;
    case GDK_KEY_x:
    case GDK_KEY_X:
        return Web::UIEvents::KeyCode::Key_X;
    case GDK_KEY_y:
    case GDK_KEY_Y:
        return Web::UIEvents::KeyCode::Key_Y;
    case GDK_KEY_z:
    case GDK_KEY_Z:
        return Web::UIEvents::KeyCode::Key_Z;
    case GDK_KEY_0:
    case GDK_KEY_parenright:
        return Web::UIEvents::KeyCode::Key_0;
    case GDK_KEY_1:
    case GDK_KEY_exclam:
        return Web::UIEvents::KeyCode::Key_1;
    case GDK_KEY_2:
    case GDK_KEY_at:
        return Web::UIEvents::KeyCode::Key_2;
    case GDK_KEY_3:
    case GDK_KEY_numbersign:
        return Web::UIEvents::KeyCode::Key_3;
    case GDK_KEY_4:
    case GDK_KEY_dollar:
        return Web::UIEvents::KeyCode::Key_4;
    case GDK_KEY_5:
    case GDK_KEY_percent:
        return Web::UIEvents::KeyCode::Key_5;
    case GDK_KEY_6:
    case GDK_KEY_asciicircum:
        return Web::UIEvents::KeyCode::Key_6;
    case GDK_KEY_7:
    case GDK_KEY_ampersand:
        return Web::UIEvents::KeyCode::Key_7;
    case GDK_KEY_8:
    case GDK_KEY_asterisk:
        return Web::UIEvents::KeyCode::Key_8;
    case GDK_KEY_9:
    case GDK_KEY_parenleft:
        return Web::UIEvents::KeyCode::Key_9;
    case GDK_KEY_minus:
    case GDK_KEY_underscore:
        return Web::UIEvents::KeyCode::Key_Minus;
    case GDK_KEY_equal:
    case GDK_KEY_plus:
        return Web::UIEvents::KeyCode::Key_Equal;
    case GDK_KEY_bracketleft:
    case GDK_KEY_braceleft:
        return Web::UIEvents::KeyCode::Key_LeftBracket;
    case GDK_KEY_bracketright:
    case GDK_KEY_braceright:
        return Web::UIEvents::KeyCode::Key_RightBracket;
    case GDK_KEY_backslash:
    case GDK_KEY_bar:
        return Web::UIEvents::KeyCode::Key_Backslash;
    case GDK_KEY_semicolon:
    case GDK_KEY_colon:
        return Web::UIEvents::KeyCode::Key_Semicolon;
    case GDK_KEY_apostrophe:
    case GDK_KEY_quotedbl:
        return Web::UIEvents::KeyCode::Key_Apostrophe;
    case GDK_KEY_grave:
    case GDK_KEY_asciitilde:
        return Web::UIEvents::KeyCode::Key_Backtick;
    case GDK_KEY_comma:
    case GDK_KEY_less:
        return Web::UIEvents::KeyCode::Key_Comma;
    case GDK_KEY_period:
    case GDK_KEY_greater:
        return Web::UIEvents::KeyCode::Key_Period;
    case GDK_KEY_slash:
    case GDK_KEY_question:
        return Web::UIEvents::KeyCode::Key_Slash;
    default:
        return Web::UIEvents::KeyCode::Key_Invalid;
    }
}

StringView standard_cursor_to_css_name(Gfx::StandardCursor cursor)
{
    switch (cursor) {
    case Gfx::StandardCursor::Hidden:
        return "none"sv;
    case Gfx::StandardCursor::Arrow:
        return "default"sv;
    case Gfx::StandardCursor::IBeam:
        return "text"sv;
    case Gfx::StandardCursor::Crosshair:
        return "crosshair"sv;
    case Gfx::StandardCursor::Hand:
        return "pointer"sv;
    case Gfx::StandardCursor::ResizeHorizontal:
        return "ew-resize"sv;
    case Gfx::StandardCursor::ResizeVertical:
        return "ns-resize"sv;
    case Gfx::StandardCursor::ResizeDiagonalTLBR:
        return "nwse-resize"sv;
    case Gfx::StandardCursor::ResizeDiagonalBLTR:
        return "nesw-resize"sv;
    case Gfx::StandardCursor::ResizeColumn:
        return "col-resize"sv;
    case Gfx::StandardCursor::ResizeRow:
        return "row-resize"sv;
    case Gfx::StandardCursor::Move:
        return "move"sv;
    case Gfx::StandardCursor::Wait:
        return "wait"sv;
    default:
        return "default"sv;
    }
}

}
