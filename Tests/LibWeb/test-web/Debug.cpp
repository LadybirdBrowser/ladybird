/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"
#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibRequests/RequestClient.h>

namespace TestWeb {

ByteBuffer strip_sgr_sequences(StringView input)
{
    // Regex equivalent: /\x1b\[[0-9;]*m/
    auto const* bytes = reinterpret_cast<unsigned char const*>(input.characters_without_null_termination());
    size_t length = input.length();

    ByteBuffer output;
    output.ensure_capacity(length);

    for (size_t i = 0; i < length; ++i) {
        if (bytes[i] == 0x1b && (i + 1) < length && bytes[i + 1] == '[') {
            size_t j = i + 2;
            while (j < length && (is_ascii_digit(bytes[j]) || bytes[j] == ';'))
                ++j;
            if (j < length && bytes[j] == 'm') {
                i = j;
                continue;
            }
        }

        output.append(static_cast<u8>(bytes[i]));
    }

    return output;
}

static bool stdin_and_stdout_are_ttys()
{
    auto stdin_is_tty_or_error = Core::System::isatty(0);
    auto stdout_is_tty_or_error = Core::System::isatty(1);

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
    file_actions.append(Core::FileAction::DupFd { .write_fd = write_fd, .fd = 1 });
    file_actions.append(Core::FileAction::DupFd { .write_fd = write_fd, .fd = 2 });
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

    MonotonicTime deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(timeout_ms);
    bool exited = false;

    for (;;) {
        struct pollfd pfd;
        pfd.fd = read_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        auto poll_result = Core::System::poll({ &pfd, 1 }, 50);
        if (!poll_result.is_error() && (pfd.revents & POLLIN)) {
            Array<u8, 4096> buffer;
            auto nread_or_error = Core::System::read(read_fd, buffer);
            if (!nread_or_error.is_error() && nread_or_error.value() > 0)
                builder.append(StringView { reinterpret_cast<char const*>(buffer.data()), nread_or_error.value() });
        }

        auto wait_result = Core::System::waitpid(process.pid(), WNOHANG);
        if (!wait_result.is_error() && wait_result.value().pid == process.pid()) {
            exited = true;
            break;
        }

        if (MonotonicTime::now() >= deadline)
            break;
    }

    if (!exited) {
        (void)Core::System::kill(process.pid(), SIGKILL);
        (void)Core::System::waitpid(process.pid(), 0);
    }

    for (;;) {
        Array<u8, 8192> buffer;
        auto nread_or_error = Core::System::read(read_fd, buffer);
        if (nread_or_error.is_error() || nread_or_error.value() == 0)
            break;
        builder.append(StringView { reinterpret_cast<char const*>(buffer.data()), nread_or_error.value() });
    }

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
