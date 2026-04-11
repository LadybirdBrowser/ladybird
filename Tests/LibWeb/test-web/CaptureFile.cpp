/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CaptureFile.h"
#include "Application.h"

#include <AK/GenericLexer.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <LibGfx/Color.h>

namespace TestWeb {

static StringBuilder convert_ansi_to_html(StringView input);

CaptureFile::CaptureFile(ByteString destination_path)
    : m_destination_path(move(destination_path))
{
    ByteString relative_destination_path = LexicalPath::relative_path(m_destination_path, Application::the().results_directory).value_or(m_destination_path);
    StringBuilder builder;
    for (char ch : relative_destination_path)
        builder.append((ch == '/' || ch == '\\') ? '_' : ch);
    builder.append(".capture"sv);
    m_writer_path = LexicalPath::join(Application::the().results_directory, builder.string_view()).string();
    if (auto maybe_writer = Core::File::open(m_writer_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !maybe_writer.is_error())
        m_writer = maybe_writer.release_value();
}

ErrorOr<bool> CaptureFile::transfer_to_output_file()
{
    bool has_content = m_writer != nullptr && m_writer->tell().value_or(0) > 0;
    m_writer = nullptr;
    if (m_writer_path.is_empty())
        return false;

    (void)Core::System::unlink(m_destination_path);
    ScopeGuard cleanup = [&] {
        (void)Core::System::unlink(m_writer_path);
        m_writer_path = {};
        m_destination_path = {};
    };
    if (!has_content)
        return false;

    auto raw_capture = TRY(Core::File::open(m_writer_path, Core::File::OpenMode::Read));
    auto converted = convert_ansi_to_html(TRY(raw_capture->read_until_eof()));
    TRY(Core::Directory::create(LexicalPath(m_destination_path).dirname(), Core::Directory::CreateDirectories::Yes));
    auto html_file = TRY(Core::File::open(m_destination_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(html_file->write_until_depleted(converted.string_view().bytes()));
    return true;
}

void CaptureFile::write(StringView message)
{
    if (m_writer && !message.is_empty())
        MUST(m_writer->write_until_depleted(message.bytes()));
}

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

static StringBuilder convert_ansi_to_html(StringView input)
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

}
