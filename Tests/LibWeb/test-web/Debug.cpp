/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"
#include "TestWeb.h"

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/GenericLexer.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibGfx/Color.h>

namespace TestWeb {

static constexpr Array s_standard_colors = {
    Color(0, 0, 0),       // Black
    Color(231, 0, 0),     // Red
    Color(0, 231, 0),     // Green
    Color(231, 196, 0),   // Yellow
    Color(35, 126, 231),  // Blue
    Color(231, 0, 231),   // Magenta
    Color(0, 231, 231),   // Cyan
    Color(255, 255, 255), // White
};

static constexpr Array s_bright_colors = [] {
    auto array = s_standard_colors;
    for (size_t i = 0; i < array.size(); i++) {
        array[i] = array[i].lightened(1.5f);
    }
    return array;
}();

static Color indexed_color_to_rgb(u32 index)
{
    if (index < 8)
        return s_standard_colors[index];
    if (index < 16)
        return s_bright_colors[index - 8];
    if (index < 232) {
        // 6x6x6 color cube
        auto n = index - 16;
        u8 r = static_cast<u8>(n / 36);
        u8 g = static_cast<u8>((n % 36) / 6);
        u8 b = static_cast<u8>(n % 6);
        return {
            static_cast<u8>(r ? 55 + (40 * r) : 0),
            static_cast<u8>(g ? 55 + (40 * g) : 0),
            static_cast<u8>(b ? 55 + (40 * b) : 0),
        };
    }
    if (index < 256) {
        // Grayscale ramp
        u8 level = static_cast<u8>(8 + (10 * (index - 232)));
        return { level, level, level };
    }
    return { Color::NamedColor::LightGray };
}

struct ANSIStyle {
    Optional<Color> foreground;
    Optional<Color> background;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;

    bool has_styles() const
    {
        return foreground.has_value() || background.has_value()
            || bold || dim || italic || underline;
    }

    void reset() { *this = {}; }
};

static void append_style_span(StringBuilder& html, ANSIStyle const& style)
{
    if (!style.has_styles())
        return;

    html.append("<span style=\""sv);
    if (style.foreground.has_value())
        html.appendff("color:{};", style.foreground->to_string());
    if (style.background.has_value())
        html.appendff("background:{};", style.background->to_string());
    if (style.bold)
        html.append("font-weight:bold;"sv);
    if (style.dim)
        html.append("opacity:0.6;"sv);
    if (style.italic)
        html.append("font-style:italic;"sv);
    if (style.underline)
        html.append("text-decoration:underline;"sv);
    html.append("\">"sv);
}

static void apply_sgr_codes(ReadonlySpan<u32> codes, ANSIStyle& style)
{
    if (codes.is_empty()) {
        style.reset();
        return;
    }

    size_t i = 0;
    auto has_codes = [&](auto count) {
        return i + count <= codes.size();
    };
    auto take_code = [&] {
        return codes[i++];
    };
    while (i < codes.size()) {
        u32 code = take_code();
        switch (code) {
        case 0:
            style.reset();
            break;
        case 1:
            style.bold = true;
            break;
        case 2:
            style.dim = true;
            break;
        case 3:
            style.italic = true;
            break;
        case 4:
            style.underline = true;
            break;
        case 22:
            style.bold = false;
            style.dim = false;
            break;
        case 23:
            style.italic = false;
            break;
        case 24:
            style.underline = false;
            break;
        case 30:
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            style.foreground = s_standard_colors[code - 30];
            break;
        case 38: {
            if (!has_codes(1))
                break;
            u32 mode = take_code();
            if (mode == 5 && has_codes(1)) {
                style.foreground = indexed_color_to_rgb(take_code());
            } else if (mode == 2 && has_codes(3)) {
                u8 r = static_cast<u8>(take_code());
                u8 g = static_cast<u8>(take_code());
                u8 b = static_cast<u8>(take_code());
                style.foreground = Color(r, g, b);
            }
            break;
        }
        case 39:
            style.foreground = {};
            break;
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            style.background = s_standard_colors[code - 40];
            break;
        case 48: {
            if (!has_codes(1))
                break;
            u32 mode = take_code();
            if (mode == 5 && has_codes(1)) {
                style.background = indexed_color_to_rgb(take_code());
            } else if (mode == 2 && has_codes(3)) {
                u8 r = static_cast<u8>(take_code());
                u8 g = static_cast<u8>(take_code());
                u8 b = static_cast<u8>(take_code());
                style.background = Color(r, g, b);
            }
            break;
        }
        case 49:
            style.background = {};
            break;
        case 90:
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            style.foreground = s_bright_colors[code - 90];
            break;
        case 100:
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            style.background = s_bright_colors[code - 100];
            break;
        default:
            break;
        }
    }
}

static bool is_csi_parameter_byte(char c)
{
    return c >= '0' && c <= '?';
}

static bool is_csi_intermediate_byte(char c)
{
    return c >= ' ' && c <= '/';
}

static bool is_csi_final_byte(char c)
{
    return c >= '@' && c <= '~';
}

StringBuilder convert_ansi_to_html(StringView input)
{
    GenericLexer lexer(input);
    StringBuilder html;

    html.append(R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
body { margin: 0; background: #0d1117; color: #e6edf3; }
pre { margin: 0; padding: 16px; font-family: ui-monospace, monospace; font-size: 12px; line-height: 1.5; }
</style>
</head>
<body><pre>)html"sv);

    ANSIStyle style;
    bool span_open = false;
    constexpr char introducer = '\x1b';

    while (!lexer.is_eof()) {
        if (lexer.consume_specific(introducer)) {
            if (lexer.consume_specific('[')) {
                // CSI sequence: parse numeric parameters, then intermediate and final bytes.
                Vector<u32, 8> codes;
                while (!lexer.is_eof() && is_csi_parameter_byte(lexer.peek())) {
                    if (auto code = lexer.consume_decimal_integer<u32>(); !code.is_error()) {
                        codes.append(code.value());
                        lexer.consume_specific(';');
                    } else if (lexer.consume_specific(';')) {
                        // Empty parameter defaults to 0.
                        codes.append(0);
                    } else {
                        // Non-digit, non-';' parameter byte (e.g. '?' in private mode sequences).
                        lexer.ignore_while(is_csi_parameter_byte);
                        break;
                    }
                }
                lexer.ignore_while(is_csi_intermediate_byte);
                if (!lexer.is_eof() && is_csi_final_byte(lexer.peek())) {
                    char final_byte = lexer.consume();
                    if (final_byte == 'm') {
                        if (span_open) {
                            html.append("</span>"sv);
                            span_open = false;
                        }
                        apply_sgr_codes(codes, style);
                        if (style.has_styles()) {
                            append_style_span(html, style);
                            span_open = true;
                        }
                    }
                }
            } else if (lexer.consume_specific(']')) {
                // OSC sequence: consume until BEL or ST.
                while (!lexer.is_eof()) {
                    if (lexer.consume_specific('\x07'))
                        break;
                    if (lexer.consume_specific("\x1b\\"sv))
                        break;
                    lexer.ignore();
                }
            } else if (!lexer.is_eof()) {
                // Other two-byte escape sequence.
                lexer.ignore();
            }
            continue;
        }

        static constexpr auto is_skipped_character = [](char c) {
            return c != introducer && is_ascii_control(c) && c != '\n' && c != '\t';
        };

        // Consume a run of visible text (plus newlines and tabs) and HTML-escape it.
        auto run = lexer.consume_until([](char c) {
            return c == introducer || is_skipped_character(c);
        });
        html.append(escape_html_entities(run));

        lexer.ignore_while(is_skipped_character);
    }

    if (span_open)
        html.append("</span>"sv);

    html.append("</pre></body></html>"sv);

    return html;
}

static void maybe_attach_lldb_to_process(pid_t pid)
{
    if (pid <= 0)
        return;

    Vector<ByteString> arguments;
    arguments.append("-p"sv);
    arguments.append(ByteString::number(pid));

    auto process_or_error = Core::Process::spawn({
        .executable = "lldb"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    });

    if (process_or_error.is_error()) {
        warnln("Failed to spawn lldb: {}", process_or_error.error());
        return;
    }

    Core::Process lldb = process_or_error.release_value();
    if (auto wait_result = lldb.wait_for_termination(); wait_result.is_error())
        warnln("Failed waiting for lldb: {}", wait_result.error());
}

static void maybe_attach_gdb_to_process(pid_t pid)
{
    if (pid <= 0)
        return;

    Vector<ByteString> arguments;
    arguments.append("-q"sv);
    arguments.append("-p"sv);
    arguments.append(ByteString::number(pid));

    auto process_or_error = Core::Process::spawn({
        .executable = "gdb"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    });

    if (process_or_error.is_error()) {
        warnln("Failed to spawn gdb: {}", process_or_error.error());
        return;
    }

    Core::Process gdb = process_or_error.release_value();
    if (auto wait_result = gdb.wait_for_termination(); wait_result.is_error())
        warnln("Failed waiting for gdb: {}", wait_result.error());
}

void maybe_attach_on_fail_fast_timeout(pid_t pid)
{
    if (pid <= 0
        || !Core::System::isatty(STDIN_FILENO).value_or(false)
        || !Core::System::isatty(STDOUT_FILENO).value_or(false))
        return;

    outln("Fail-fast timeout in WebContent pid {}.", pid);
    outln("You may attach a debugger now (test-web will wait)."sv);
    outln("- Press Enter to continue shutdown + exit"sv);
    outln("- Type 'gdb' then Enter to attach with gdb first"sv);
    outln("- Type 'lldb' then Enter to attach with lldb first"sv);
    MUST(Core::System::write(1, "> "sv.bytes()));

    auto standard_input_or_error = Core::File::standard_input();
    if (standard_input_or_error.is_error())
        return;

    Array<u8, 64> input_buffer {};
    auto buffered_standard_input_or_error = Core::InputBufferedFile::create(standard_input_or_error.release_value());
    if (buffered_standard_input_or_error.is_error())
        return;

    auto& buffered_standard_input = buffered_standard_input_or_error.value();
    auto response_or_error = buffered_standard_input->read_line(input_buffer);
    if (response_or_error.is_error())
        return;

    ByteString response { response_or_error.value() };
    response = response.trim_whitespace();
    if (response.equals_ignoring_ascii_case("gdb"sv)) {
        maybe_attach_gdb_to_process(pid);
        return;
    }
    if (response.equals_ignoring_ascii_case("lldb"sv))
        maybe_attach_lldb_to_process(pid);
}

}
