/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Utf8View.h>
#include <LibWebView/TerminalInputParser.h>

namespace WebView {

using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;
using Web::UIEvents::MouseButton;

static constexpr u8 escape = 0x1b;
static constexpr auto bracketed_paste_end = "\x1b[201~"sv;

// The modifier bits of the keyboard protocol, which SGR mouse reports carry two bits higher and without Super.
static KeyModifier modifiers_from_terminal_bits(u32 bits)
{
    auto modifiers = KeyModifier::Mod_None;
    if (bits & 1)
        modifiers |= KeyModifier::Mod_Shift;
    if (bits & 2)
        modifiers |= KeyModifier::Mod_Alt;
    if (bits & 4)
        modifiers |= KeyModifier::Mod_Ctrl;
    if (bits & 8)
        modifiers |= KeyModifier::Mod_Super;
    return modifiers;
}

static TerminalKeyReport key_press(KeyCode key, u32 code_point = 0, KeyModifier modifiers = KeyModifier::Mod_None)
{
    return { .key = key, .code_point = code_point, .modifiers = modifiers };
}

static TerminalKeyReport legacy_encoded(TerminalKeyReport report)
{
    report.is_legacy_encoding = true;
    return report;
}

// A key the keyboard protocol reports by number: a code point, or one of its private-use numbers for keys without one.
static TerminalKeyReport key_press_for_key_number(u32 number)
{
    switch (number) {
    case 0x1b:
        return key_press(KeyCode::Key_Escape, 0x1b);
    case '\r':
        return key_press(KeyCode::Key_Return, '\n');
    case '\t':
        return key_press(KeyCode::Key_Tab, '\t');
    case 0x7f:
    case '\b':
        return key_press(KeyCode::Key_Backspace, '\b');
    case 57358:
        return key_press(KeyCode::Key_CapsLock);
    case 57359:
        return key_press(KeyCode::Key_ScrollLock);
    case 57360:
        return key_press(KeyCode::Key_NumLock);
    case 57361:
        return key_press(KeyCode::Key_PrintScreen);
    case 57362:
        return key_press(KeyCode::Key_PauseBreak);
    case 57363:
        return key_press(KeyCode::Key_Menu);
    case 57441:
        return key_press(KeyCode::Key_LeftShift);
    case 57442:
        return key_press(KeyCode::Key_LeftControl);
    case 57443:
        return key_press(KeyCode::Key_LeftAlt);
    case 57444:
        return key_press(KeyCode::Key_LeftSuper);
    case 57447:
        return key_press(KeyCode::Key_RightShift);
    case 57448:
        return key_press(KeyCode::Key_RightControl);
    case 57449:
        return key_press(KeyCode::Key_RightAlt);
    case 57450:
        return key_press(KeyCode::Key_RightSuper);
    default:
        break;
    }
    // The rest of the private-use area holds keys this view has no key code for.
    if (number >= 0xe000 && number < 0xf900)
        return key_press(KeyCode::Key_Invalid);
    return key_press(Web::UIEvents::code_point_to_key_code(number), number);
}

// The keys of CSI 1;<modifiers> <letter> and SS3 <letter>.
static KeyCode key_for_letter(u8 letter)
{
    switch (letter) {
    case 'A':
        return KeyCode::Key_Up;
    case 'B':
        return KeyCode::Key_Down;
    case 'C':
        return KeyCode::Key_Right;
    case 'D':
        return KeyCode::Key_Left;
    case 'H':
        return KeyCode::Key_Home;
    case 'F':
        return KeyCode::Key_End;
    case 'P':
        return KeyCode::Key_F1;
    case 'Q':
        return KeyCode::Key_F2;
    case 'S':
        return KeyCode::Key_F4;
    default:
        return KeyCode::Key_Invalid;
    }
}

// The keys of CSI <number>;<modifiers> ~.
static KeyCode key_for_tilde_number(u32 number)
{
    switch (number) {
    case 1:
    case 7:
        return KeyCode::Key_Home;
    case 2:
        return KeyCode::Key_Insert;
    case 3:
        return KeyCode::Key_Delete;
    case 4:
    case 8:
        return KeyCode::Key_End;
    case 5:
        return KeyCode::Key_PageUp;
    case 6:
        return KeyCode::Key_PageDown;
    case 11:
        return KeyCode::Key_F1;
    case 12:
        return KeyCode::Key_F2;
    case 13:
        return KeyCode::Key_F3;
    case 14:
        return KeyCode::Key_F4;
    case 15:
        return KeyCode::Key_F5;
    case 17:
        return KeyCode::Key_F6;
    case 18:
        return KeyCode::Key_F7;
    case 19:
        return KeyCode::Key_F8;
    case 20:
        return KeyCode::Key_F9;
    case 21:
        return KeyCode::Key_F10;
    case 23:
        return KeyCode::Key_F11;
    case 24:
        return KeyCode::Key_F12;
    default:
        return KeyCode::Key_Invalid;
    }
}

// "<modifiers>[:<event type>]", where the modifiers are one more than their bits and the event type is 1 for a press,
// 2 for a repeat and 3 for a release.
static void apply_modifier_field(StringView field, TerminalKeyReport& report)
{
    if (field.is_empty())
        return;
    auto parts = field.split_view(':', SplitBehavior::KeepEmpty);
    if (auto modifiers = parts[0].to_number<u32>(); modifiers.has_value() && *modifiers > 0)
        report.modifiers = modifiers_from_terminal_bits(*modifiers - 1);
    if (parts.size() < 2)
        return;
    switch (parts[1].to_number<u32>().value_or(1)) {
    case 2:
        report.phase = TerminalKeyReport::Phase::Repeat;
        break;
    case 3:
        report.phase = TerminalKeyReport::Phase::Release;
        break;
    default:
        report.phase = TerminalKeyReport::Phase::Press;
        break;
    }
}

static Optional<TerminalMouseReport> mouse_report(u32 button_code, Gfx::IntPoint position, bool is_release)
{
    static constexpr u32 motion_bit = 32;
    static constexpr u32 wheel_bit = 64;
    static constexpr u32 extra_button_bit = 128;

    TerminalMouseReport report { .modifiers = modifiers_from_terminal_bits((button_code >> 2) & 7), .position = position };
    auto low_bits = static_cast<int>(button_code & 3);

    if (button_code & wheel_bit) {
        report.kind = TerminalMouseReport::Kind::Wheel;
        report.wheel_steps = low_bits < 2 ? Gfx::IntPoint { 0, low_bits == 0 ? -1 : 1 } : Gfx::IntPoint { low_bits == 2 ? -1 : 1, 0 };
        return report;
    }
    if (button_code & motion_bit) {
        report.kind = TerminalMouseReport::Kind::Motion;
        return report;
    }
    // Without the extra-button bit, 3 is a legacy release of no button in particular.
    if (low_bits == 3 && !(button_code & extra_button_bit))
        return {};
    report.button = Web::UIEvents::button_code_to_mouse_button(static_cast<i16>(low_bits + (button_code & extra_button_bit ? 3 : 0)));
    if (report.button == MouseButton::None)
        return {};
    report.kind = is_release ? TerminalMouseReport::Kind::Release : TerminalMouseReport::Kind::Press;
    return report;
}

Optional<size_t> TerminalInputParser::parse_control_sequence(ReadonlyBytes bytes, Vector<TerminalInput>& inputs)
{
    size_t index = 2;
    while (index < bytes.size() && bytes[index] >= 0x30 && bytes[index] <= 0x3f)
        ++index;
    auto parameters_end = index;
    while (index < bytes.size() && bytes[index] >= 0x20 && bytes[index] <= 0x2f)
        ++index;
    if (index >= bytes.size())
        return {};

    auto final_byte = bytes[index];
    size_t consumed = index + 1;
    if (final_byte < 0x40 || final_byte > 0x7e)
        return consumed;

    StringView parameters { bytes.slice(2, parameters_end - 2) };
    auto has_intermediates = parameters_end != index;

    if (parameters.starts_with('<') && (final_byte == 'M' || final_byte == 'm')) {
        auto fields = parameters.substring_view(1).split_view(';');
        if (fields.size() != 3)
            return consumed;
        auto button_code = fields[0].to_number<u32>();
        auto x = fields[1].to_number<int>();
        auto y = fields[2].to_number<int>();
        if (!button_code.has_value() || !x.has_value() || !y.has_value())
            return consumed;
        // Pixel coordinates count from one.
        if (auto report = mouse_report(*button_code, { *x - 1, *y - 1 }, final_byte == 'm'); report.has_value())
            inputs.append(report.release_value());
        return consumed;
    }

    if (final_byte == '~' && parameters == "200"sv) {
        m_paste = ByteBuffer {};
        return consumed;
    }

    if (parameters.is_empty() && (final_byte == 'I' || final_byte == 'O')) {
        inputs.append(TerminalFocusChange { .has_focus = final_byte == 'I' });
        return consumed;
    }

    if (parameters.starts_with('?') && final_byte == 'u' && !has_intermediates) {
        inputs.append(TerminalKeyboardProtocolFlags { .flags = parameters.substring_view(1).to_number<u32>().value_or(0) });
        return consumed;
    }

    // Replies to queries this parser has no use for, such as DECRQM and device attributes.
    if (has_intermediates || parameters.starts_with('?') || parameters.starts_with('>') || parameters.starts_with('='))
        return consumed;

    auto fields = parameters.split_view(';', SplitBehavior::KeepEmpty);

    if (final_byte == 't') {
        if (fields.size() == 3 && fields[0] == "4"sv) {
            auto height = fields[1].to_number<int>();
            auto width = fields[2].to_number<int>();
            if (height.has_value() && width.has_value())
                inputs.append(TerminalPixelSizeReport { .size = { *width, *height } });
        }
        return consumed;
    }

    TerminalKeyReport report;
    if (final_byte == 'u') {
        auto key_numbers = fields.is_empty() ? Vector<StringView> {} : fields[0].split_view(':', SplitBehavior::KeepEmpty);
        auto key_number = key_numbers.is_empty() ? Optional<u32> {} : key_numbers[0].to_number<u32>();
        if (!key_number.has_value())
            return consumed;
        report = key_press_for_key_number(*key_number);
        if (key_numbers.size() > 1)
            report.shifted_code_point = key_numbers[1].to_number<u32>();
        if (fields.size() > 2) {
            for (auto text : fields[2].split_view(':')) {
                if (auto code_point = text.to_number<u32>(); code_point.has_value())
                    report.text_code_points.append(*code_point);
            }
        }
    } else if (final_byte == '~') {
        auto number = fields.is_empty() ? Optional<u32> {} : fields[0].to_number<u32>();
        report = key_press(number.has_value() ? key_for_tilde_number(*number) : KeyCode::Key_Invalid);
    } else {
        report = key_press(key_for_letter(final_byte));
    }
    if (report.key == KeyCode::Key_Invalid)
        return consumed;
    if (fields.size() > 1)
        apply_modifier_field(fields[1], report);
    inputs.append(move(report));
    return consumed;
}

Optional<size_t> TerminalInputParser::parse_one(ReadonlyBytes bytes, Vector<TerminalInput>& inputs)
{
    if (m_paste.has_value()) {
        if (auto end = StringView { bytes }.find(bracketed_paste_end); end.has_value()) {
            m_paste->append(bytes.slice(0, *end));
            inputs.append(TerminalPaste { .text = ByteString { m_paste->bytes() } });
            m_paste.clear();
            return *end + bracketed_paste_end.length();
        }

        // Keep what may be the start of the end marker for the next chunk.
        size_t kept = 0;
        for (size_t length = min(bracketed_paste_end.length() - 1, bytes.size()); length > 0; --length) {
            if (StringView { bytes.slice(bytes.size() - length) } == bracketed_paste_end.substring_view(0, length)) {
                kept = length;
                break;
            }
        }
        m_paste->append(bytes.slice(0, bytes.size() - kept));
        if (kept == bytes.size())
            return {};
        return bytes.size() - kept;
    }

    if (bytes[0] == escape) {
        if (bytes.size() < 2)
            return {};

        switch (bytes[1]) {
        case '[':
            return parse_control_sequence(bytes, inputs);

        case 'O': {
            if (bytes.size() < 3)
                return {};
            auto key = bytes[2] == 'R' ? KeyCode::Key_F3 : key_for_letter(bytes[2]);
            if (key != KeyCode::Key_Invalid)
                inputs.append(legacy_encoded(key_press(key)));
            return 3;
        }

        case '_': {
            auto end = StringView { bytes }.find("\x1b\\"sv, 2);
            if (!end.has_value())
                return {};
            StringView body { bytes.slice(2, *end - 2) };
            if (body.starts_with('G')) {
                inputs.append(TerminalGraphicsReply { .text = body.substring_view(1) });
            } else if (body.starts_with("ladybird;"sv)) {
                auto command = body.substring_view("ladybird;"sv.length());
                auto separator = command.find(';');
                if (separator.has_value())
                    inputs.append(TerminalCommand { .name = command.substring_view(0, *separator), .argument = command.substring_view(*separator + 1) });
                else
                    inputs.append(TerminalCommand { .name = command, .argument = {} });
            }
            return *end + 2;
        }

        case ']':
        case 'P': {
            // An OSC or DCS reply, which ends with BEL (OSC only) or ST.
            for (size_t index = 2; index < bytes.size(); ++index) {
                if (bytes[1] == ']' && bytes[index] == 0x07)
                    return index + 1;
                if (bytes[index] == escape && index + 1 < bytes.size() && bytes[index + 1] == '\\')
                    return index + 2;
            }
            return {};
        }

        default: {
            // Legacy Alt+key: the key, with ESC before it.
            Vector<TerminalInput> key_inputs;
            auto key_consumed = parse_one(bytes.slice(1), key_inputs);
            if (!key_consumed.has_value())
                return {};
            for (auto& input : key_inputs) {
                if (auto* key = input.get_pointer<TerminalKeyReport>()) {
                    key->modifiers |= KeyModifier::Mod_Alt;
                    key->is_legacy_encoding = true;
                }
                inputs.append(move(input));
            }
            return 1 + *key_consumed;
        }
        }
    }

    // Legacy bytes: UTF-8 text and control characters.
    auto lead = bytes[0];
    size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2
        : (lead >> 4) == 0xe                             ? 3
        : (lead >> 3) == 0x1e                            ? 4
                                                         : 1;
    if (bytes.size() < length)
        return {};

    Utf8View view { StringView { bytes.slice(0, length) } };
    if (!view.validate())
        return length;
    auto code_point = *view.begin();

    TerminalKeyReport report;
    if (code_point == 0x00) {
        report = key_press(KeyCode::Key_Space, ' ', KeyModifier::Mod_Ctrl);
    } else if (code_point >= 0x01 && code_point <= 0x1a && code_point != '\t' && code_point != '\r' && code_point != '\n' && code_point != '\b') {
        auto letter = 'a' + code_point - 1;
        report = key_press(Web::UIEvents::code_point_to_key_code(letter), letter, KeyModifier::Mod_Ctrl);
    } else if (code_point >= 0x1c && code_point <= 0x1f) {
        auto character = "\\]^_"sv[code_point - 0x1c];
        report = key_press(Web::UIEvents::code_point_to_key_code(character), character, KeyModifier::Mod_Ctrl);
    } else {
        report = key_press_for_key_number(code_point == '\n' ? '\r' : code_point);
        if (code_point >= ' ' && code_point != 0x7f)
            report.text_code_points.append(code_point);
    }
    if (report.key != KeyCode::Key_Invalid)
        inputs.append(legacy_encoded(move(report)));
    return length;
}

Vector<TerminalInput> TerminalInputParser::parse(ReadonlyBytes chunk)
{
    Vector<TerminalInput> inputs;
    m_pending.append(chunk);

    // Without the keyboard protocol, the Escape key arrives as a lone ESC. Anything else that starts with one arrives
    // with the rest of its sequence.
    if (!m_paste.has_value() && m_pending.size() == 1 && m_pending[0] == escape) {
        m_pending.clear();
        inputs.append(legacy_encoded(key_press_for_key_number(escape)));
        return inputs;
    }

    auto bytes = m_pending.bytes();
    size_t offset = 0;
    while (offset < bytes.size()) {
        auto consumed = parse_one(bytes.slice(offset), inputs);
        if (!consumed.has_value())
            break;
        offset += *consumed;
    }

    m_pending = MUST(ByteBuffer::copy(bytes.slice(offset)));
    return inputs;
}

}
