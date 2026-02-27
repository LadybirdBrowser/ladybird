/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"
#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/GenericLexer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibRequests/RequestClient.h>

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

static bool stdin_and_stdout_are_ttys()
{
    auto stdin_is_tty_or_error = Core::System::isatty(STDIN_FILENO);
    auto stdout_is_tty_or_error = Core::System::isatty(STDOUT_FILENO);

    return !stdin_is_tty_or_error.is_error() && stdin_is_tty_or_error.value()
        && !stdout_is_tty_or_error.is_error() && stdout_is_tty_or_error.value();
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

static void append_diagnostics_header(StringBuilder& builder, Test const& test, size_t view_id, StringView current_url)
{
    auto& app = Application::the();
    builder.appendff("\n==== timeout diagnostics ====\n");
    builder.appendff("time: {}\n", UnixDateTime::now().to_byte_string());
    builder.appendff("test: {}\n", test.relative_path);
    builder.appendff("run: {}/{}\n", test.run_index, test.total_runs);
    builder.appendff("view: {}\n", view_id);
    builder.appendff("test-concurrency: {}\n", app.test_concurrency);
    builder.appendff("current-url: {}\n\n", current_url);
}

static Optional<String> request_page_info_with_timeout(TestWebView& view, WebView::PageInfoType page_info_type, u32 timeout_ms)
{
    struct PageInfoState : public RefCounted<PageInfoState> {
        Optional<String> text;
        bool finished { false };
        bool timed_out { false };
    };
    auto state = make_ref_counted<PageInfoState>();
    auto timeout_timer = Core::Timer::create_single_shot(static_cast<int>(timeout_ms), [state] {
        state->timed_out = true;
    });

    auto promise = view.request_internal_page_info(page_info_type);
    promise->when_resolved([state](String& resolved) {
        if (state->timed_out)
            return;
        state->text = resolved;
        state->finished = true;
    });

    timeout_timer->start();
    Core::EventLoop::current().spin_until([state] {
        return state->finished || state->timed_out;
    });

    return state->text;
}

static void append_page_info(StringBuilder& builder, StringView title, Optional<String> const& text)
{
    builder.appendff("---- {} ----\n", title);
    if (text.has_value()) {
        builder.append(text->bytes_as_string_view());
        if (!text->bytes_as_string_view().ends_with('\n'))
            builder.append("\n"sv);
    } else {
        builder.append("(Timed out waiting for page info)\n"sv);
    }
    builder.append("\n"sv);
}

static ErrorOr<void> run_tool_and_append_output(StringBuilder& builder, StringView tool_name, Vector<ByteString> const& arguments, u32 timeout_ms)
{
#ifdef AK_OS_WINDOWS
    (void)builder;
    (void)tool_name;
    (void)arguments;
    (void)timeout_ms;
    return Error::from_string_literal("No Windows yet");
#else
    auto pipe_fds = TRY(Core::System::pipe2(0));
    int read_fd = pipe_fds[0];
    int write_fd = pipe_fds[1];

    Vector<Core::ProcessSpawnOptions::FileActionType> file_actions;
    file_actions.append(Core::FileAction::CloseFile { .fd = read_fd });
    file_actions.append(Core::FileAction::DupFd { .write_fd = write_fd, .fd = STDOUT_FILENO });
    file_actions.append(Core::FileAction::DupFd { .write_fd = write_fd, .fd = STDERR_FILENO });
    file_actions.append(Core::FileAction::CloseFile { .fd = write_fd });

    auto process_or_error = Core::Process::spawn({
        .executable = tool_name,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = move(file_actions),
    });

    if (process_or_error.is_error()) {
        (void)Core::System::close(read_fd);
        (void)Core::System::close(write_fd);
        return process_or_error.release_error();
    }

    Core::Process process = process_or_error.release_value();
    (void)Core::System::close(write_fd);

    bool finished = false;
    bool timed_out = false;

    auto notifier = Core::Notifier::construct(read_fd, Core::Notifier::Type::Read);
    notifier->on_activation = [&] {
        Array<u8, 4096> buffer;
        auto nread_or_error = Core::System::read(read_fd, buffer);
        if (nread_or_error.is_error() || nread_or_error.value() == 0) {
            finished = true;
            return;
        }
        builder.append(StringView { buffer.data(), nread_or_error.value() });
    };

    auto timeout_timer = Core::Timer::create_single_shot(static_cast<int>(timeout_ms), [&] {
        timed_out = true;
    });
    timeout_timer->start();

    Core::EventLoop::current().spin_until([&] {
        return finished || timed_out;
    });

    notifier->close();

    if (!finished)
        (void)Core::System::kill(process.pid(), SIGKILL);

    (void)Core::System::waitpid(process.pid(), 0);
    (void)Core::System::close(read_fd);
    return {};
#endif
}

static bool tool_exists_on_path(StringView tool_name)
{
    StringView path_view = Core::Environment::get("PATH"sv).value_or(DEFAULT_PATH_SV);

    for (StringView dir : path_view.split_view(':')) {
        if (dir.is_empty())
            continue;
        ByteString candidate = LexicalPath::join(dir, tool_name).string();
        if (!FileSystem::exists(candidate))
            continue;
        if (FileSystem::is_directory(candidate))
            continue;
        return true;
    }
    return false;
}

enum class BacktraceTool : u8 {
    None,
    LLDB,
    GDB,
    Sample,
};

static BacktraceTool choose_backtrace_tool_for_process(pid_t)
{
#if defined(AK_OS_MACOS)
    if (tool_exists_on_path("lldb"sv))
        return BacktraceTool::LLDB;
    if (tool_exists_on_path("sample"sv))
        return BacktraceTool::Sample;
    if (tool_exists_on_path("gdb"sv))
        return BacktraceTool::GDB;
    return BacktraceTool::None;
#else
    bool have_lldb = tool_exists_on_path("lldb"sv);
    bool have_gdb = tool_exists_on_path("gdb"sv);

    if (have_lldb && have_gdb) {
#    ifdef AK_COMPILER_CLANG
        return BacktraceTool::LLDB;
#    else
        return BacktraceTool::GDB;
#    endif
    }

    if (have_lldb)
        return BacktraceTool::LLDB;
    if (have_gdb)
        return BacktraceTool::GDB;
    return BacktraceTool::None;
#endif
}

static void append_backtrace_for_process(StringBuilder& builder, StringView process_kind, pid_t pid)
{
    builder.appendff("---- {} pid {} stacks ----\n", process_kind, pid);

    if (pid <= 0) {
        builder.append("(No pid)\n\n"sv);
        return;
    }

    switch (choose_backtrace_tool_for_process(pid)) {

    case BacktraceTool::LLDB: {
        constexpr u32 backtrace_timeout_ms = 60 * 1000; // DWARF indexing on mac can take ages
        Vector<ByteString> arguments;
        arguments.append("--no-lldbinit"sv);
        arguments.append("-b"sv);
        arguments.append("-p"sv);
        arguments.append(ByteString::number(pid));
        arguments.append("-o"sv);
#if defined(AK_OS_MACOS)
        // On macOS, "thread backtrace all" can be slow due to expensive debug info lookups.
        arguments.append("thread backtrace -c 50 unique"sv);
#else
        arguments.append("thread backtrace all"sv);
#endif
        arguments.append("-o"sv);
        arguments.append("detach"sv);
        arguments.append("-o"sv);
        arguments.append("quit"sv);

        builder.append("[lldb]\n"sv);
        StringBuilder lldb_output;
        auto result = run_tool_and_append_output(lldb_output, "lldb"sv, arguments, backtrace_timeout_ms);
        if (result.is_error()) {
            builder.appendff("(lldb failed: {})\n", result.error());
            break;
        }
        builder.append(lldb_output.string_view());
        break;
    }
    case BacktraceTool::GDB: {
        constexpr u32 backtrace_timeout_ms = 2500;
        Vector<ByteString> arguments;
        arguments.append("-q"sv);
        arguments.append("-n"sv);
        arguments.append("-batch"sv);
        arguments.append("-p"sv);
        arguments.append(ByteString::number(pid));
        arguments.append("-ex"sv);
        arguments.append("set pagination off"sv);
        arguments.append("-ex"sv);
        arguments.append("thread apply all bt full"sv);
        arguments.append("-ex"sv);
        arguments.append("detach"sv);
        arguments.append("-ex"sv);
        arguments.append("quit"sv);

        builder.append("[gdb]\n"sv);
        if (auto result = run_tool_and_append_output(builder, "gdb"sv, arguments, backtrace_timeout_ms); result.is_error())
            builder.appendff("(gdb failed: {})\n", result.error());
        break;
    }
    case BacktraceTool::Sample: {
        constexpr u32 backtrace_timeout_ms = 2500;
        Vector<ByteString> arguments;
        arguments.append(ByteString::number(pid));
        arguments.append("1"sv);
        arguments.append("1"sv);

        builder.append("[sample]\n"sv);
        if (auto result = run_tool_and_append_output(builder, "sample"sv, arguments, backtrace_timeout_ms); result.is_error())
            builder.appendff("(sample failed: {})\n", result.error());
        break;
    }
    case BacktraceTool::None:
        builder.append("(no supported backtrace tool found on PATH)\n"sv);
        break;
    }

    builder.append("\n"sv);
}

void maybe_attach_on_fail_fast_timeout(pid_t pid)
{
    if (pid <= 0)
        return;
    if (!stdin_and_stdout_are_ttys())
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

void append_timeout_diagnostics_to_stderr(StringBuilder& stderr_builder, TestWebView& view, Test const& test, size_t view_id)
{
    append_diagnostics_header(stderr_builder, test, view_id, view.url().to_byte_string());

    append_page_info(stderr_builder, "PageInfoType::Text"sv, request_page_info_with_timeout(view, WebView::PageInfoType::Text, 750));
    append_page_info(stderr_builder, "PageInfoType::LayoutTree"sv, request_page_info_with_timeout(view, WebView::PageInfoType::LayoutTree, 750));

    append_backtrace_for_process(stderr_builder, "webcontent"sv, view.web_content_pid());
}

void append_timeout_backtraces_to_stderr(StringBuilder& stderr_builder, TestWebView& view, Test const& test, size_t view_id)
{
    append_diagnostics_header(stderr_builder, test, view_id, view.url().to_byte_string());
    append_backtrace_for_process(stderr_builder, "webcontent"sv, view.web_content_pid());
}

}
