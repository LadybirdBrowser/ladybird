/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>
#include <LibWebView/Forward.h>

namespace WebView {

// An SGR mouse report with pixel coordinates (modes 1006 and 1016), from the pane's top-left pixel.
struct TerminalMouseReport {
    enum class Kind : u8 {
        Press,
        Release,
        Motion,
        Wheel,
    };

    Kind kind { Kind::Motion };
    Web::UIEvents::MouseButton button { Web::UIEvents::MouseButton::None };
    // For a wheel report, the direction it scrolls in: -1, 0 or 1 on each axis.
    Gfx::IntPoint wheel_steps {};
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    Gfx::IntPoint position {};
};

// A key, as the Kitty keyboard protocol reports it, or as legacy bytes do.
struct TerminalKeyReport {
    enum class Phase : u8 {
        Press,
        Repeat,
        Release,
    };

    Web::UIEvents::KeyCode key { Web::UIEvents::KeyCode::Key_Invalid };
    // The character of the key without Shift, as a key event carries it; zero for keys without one, like arrows.
    u32 code_point { 0 };
    Optional<u32> shifted_code_point {};
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    Phase phase { Phase::Press };
    Vector<u32> text_code_points {};
    // Legacy encodings, such as plain bytes, report presses only, even from a terminal that reports releases.
    bool is_legacy_encoding { false };
};

struct TerminalPaste {
    ByteString text {};
};

struct TerminalFocusChange {
    bool has_focus { false };
};

// A command sent to the pane's input as ESC _ ladybird;<name>;<argument> ESC \, which no key types.
struct TerminalCommand {
    ByteString name {};
    ByteString argument {};
};

struct TerminalGraphicsReply {
    ByteString text {};
};

struct TerminalKeyboardProtocolFlags {
    u32 flags { 0 };
};

struct TerminalPixelSizeReport {
    Gfx::IntSize size {};
};

using TerminalInput = Variant<TerminalMouseReport, TerminalKeyReport, TerminalPaste, TerminalFocusChange, TerminalCommand, TerminalGraphicsReply, TerminalKeyboardProtocolFlags, TerminalPixelSizeReport>;

// Turns the bytes a terminal sends into input. A chunk can end partway through a sequence; the rest is kept for the
// next chunk.
class WEBVIEW_API TerminalInputParser {
public:
    Vector<TerminalInput> parse(ReadonlyBytes chunk);

private:
    // Each returns how many bytes it consumed, or nothing when the sequence continues past the end of the bytes.
    Optional<size_t> parse_one(ReadonlyBytes, Vector<TerminalInput>&);
    Optional<size_t> parse_control_sequence(ReadonlyBytes, Vector<TerminalInput>&);

    ByteBuffer m_pending;
    Optional<ByteBuffer> m_paste;
};

}
