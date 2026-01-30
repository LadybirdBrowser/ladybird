/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Debug.h"
#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/JsonObject.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/WebContentClient.h>

#ifndef AK_OS_WINDOWS
#    include <errno.h>
#    include <unistd.h>
#endif

namespace TestWeb {

static HashMap<WebView::ViewImplementation const*, OwnPtr<ViewOutputCapture>> s_output_captures;

struct ExtraProcessCapture {
    ByteString label;
    ByteString file_prefix;
    pid_t pid { -1 };
    OwnPtr<ViewOutputCapture> capture;
};

struct ExtraCaptureState {
    Vector<ExtraProcessCapture> captures;
    RefPtr<Core::Timer> probe_timer;
    ByteString tmp_dir;
    size_t test_index { 0 };
    bool capture_audio_server { false };
};

static HashMap<WebView::ViewImplementation const*, ExtraCaptureState> s_extra_output_captures;

struct TempFile {
    NonnullOwnPtr<Core::File> file;
    ByteString path;
};

struct MkstempResult {
    int fd { -1 };
    ByteString path;
};

static ErrorOr<MkstempResult> create_mkstemp(StringView directory, StringView prefix)
{
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));

    auto pattern = ByteString::formatted("{}/{}.XXXXXX", directory, prefix);
    Vector<char> pattern_buffer;
    pattern_buffer.ensure_capacity(pattern.length() + 1);
    pattern_buffer.append(pattern.characters(), pattern.length());
    pattern_buffer.append('\0');

    int fd = TRY(Core::System::mkstemp(pattern_buffer.span()));
    return MkstempResult { .fd = fd, .path = ByteString { pattern_buffer.data() } };
}

static ErrorOr<TempFile> create_temp_file_for_write(StringView directory, StringView prefix)
{
    auto temp = TRY(create_mkstemp(directory, prefix));
    auto file = TRY(Core::File::adopt_fd(temp.fd, Core::File::OpenMode::Write));
    return TempFile { NonnullOwnPtr<Core::File> { move(file) }, move(temp.path) };
}

ErrorOr<ByteString> create_temp_file_path(StringView directory, StringView prefix)
{
    auto temp = TRY(create_mkstemp(directory, prefix));
    TRY(Core::System::close(temp.fd));
    return move(temp.path);
}

static void drain_fds_to_temp_files(ViewOutputCapture& capture);

static OwnPtr<ViewOutputCapture> create_output_capture_for_process(pid_t pid, bool tee_to_terminal)
{
    auto process = Application::the().find_process(pid);
    if (!process.has_value())
        return nullptr;

    auto& output_capture = process->output_capture();
    if (!output_capture.stdout_file && !output_capture.stderr_file)
        return nullptr;

    auto capture = make<ViewOutputCapture>();
    capture->tee_to_terminal = tee_to_terminal;

    if (output_capture.stdout_file) {
        auto fd = output_capture.stdout_file->fd();
        capture->stdout_fd = fd;
        capture->stdout_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        capture->stdout_notifier->on_activation = [fd, capture = capture.ptr()]() {
            (void)fd;
            drain_fds_to_temp_files(*capture);
        };
    }

    if (output_capture.stderr_file) {
        auto fd = output_capture.stderr_file->fd();
        capture->stderr_fd = fd;
        capture->stderr_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        capture->stderr_notifier->on_activation = [fd, capture = capture.ptr()]() {
            (void)fd;
            drain_fds_to_temp_files(*capture);
        };
    }

    return capture;
}

static ErrorOr<void> begin_output_capture_for_process(ViewOutputCapture& capture, StringView tmp_dir, StringView prefix_base)
{
    capture.stdout_temp_file = nullptr;
    capture.stderr_temp_file = nullptr;
    capture.stdout_temp_path = {};
    capture.stderr_temp_path = {};

    if (capture.stdout_fd >= 0) {
        auto temp = TRY(create_temp_file_for_write(tmp_dir, ByteString::formatted("{}.stdout", prefix_base)));
        capture.stdout_temp_path = move(temp.path);
        capture.stdout_temp_file = move(temp.file);
        capture.stdout_sgr_stripper.reset();
    }

    if (capture.stderr_fd >= 0) {
        auto temp = TRY(create_temp_file_for_write(tmp_dir, ByteString::formatted("{}.stderr", prefix_base)));
        capture.stderr_temp_path = move(temp.path);
        capture.stderr_temp_file = move(temp.file);
        capture.stderr_sgr_stripper.reset();
    }

    return {};
}

static Optional<pid_t> webaudio_worker_pid_for_view(TestWebView& view)
{
    Optional<pid_t> pid;
    WebView::WebContentClient::for_each_client([&](WebView::WebContentClient& client) {
        if (client.pid() != view.web_content_pid())
            return IterationDecision::Continue;
        pid = client.webaudio_worker_pid_for_page_id(view.page_id());
        return IterationDecision::Break;
    });
    return pid;
}

static Optional<pid_t> audio_server_pid()
{
    auto json = WebView::Application::process_manager().serialize_json();
    if (!json.is_array())
        return {};

    auto const& array = json.as_array();
    for (auto const& entry : array.values()) {
        if (!entry.is_object())
            continue;
        auto const& object = entry.as_object();
        auto name = object.get_string("name"sv);
        if (!name.has_value())
            continue;
        if (!name->bytes_as_string_view().starts_with("AudioServer"sv))
            continue;
        auto pid_value = object.get_i64("pid"sv);
        if (!pid_value.has_value())
            continue;
        return static_cast<pid_t>(pid_value.value());
    }

    return {};
}

static void drain_fds_to_temp_files(ViewOutputCapture& capture)
{
#ifndef AK_OS_WINDOWS
    bool stdout_reached_eof_or_error = false;
    bool stderr_reached_eof_or_error = false;

    auto write_stripped_sgr_to_file = [&](Core::File& temp_file, ReadonlyBytes bytes, SgrStripperState& stripper) {
        Array<u8, 4096> out;
        size_t out_length = 0;

        auto flush_out = [&] {
            if (out_length == 0)
                return;
            (void)temp_file.write_until_depleted({ out.data(), out_length });
            out_length = 0;
        };

        auto append_out = [&](u8 byte) {
            out[out_length++] = byte;
            if (out_length == out.size())
                flush_out();
        };

        auto flush_pending_as_literal = [&] {
            for (size_t i = 0; i < stripper.pending_length; ++i)
                append_out(stripper.pending_bytes[i]);
            stripper.pending_length = 0;
        };

        auto append_pending_or_flush = [&](u8 byte) {
            if (stripper.pending_length < stripper.pending_bytes.size()) {
                stripper.pending_bytes[stripper.pending_length++] = byte;
                return;
            }

            // Unusually long sequence: treat pending as literal.
            flush_pending_as_literal();
            stripper.mode = SgrStripperState::Mode::Normal;
            append_out(byte);
        };

        for (u8 byte : bytes) {
            switch (stripper.mode) {
            case SgrStripperState::Mode::Normal:
                if (byte == 0x1b) {
                    stripper.mode = SgrStripperState::Mode::SawEsc;
                    stripper.pending_length = 0;
                    append_pending_or_flush(byte);
                    break;
                }
                append_out(byte);
                break;

            case SgrStripperState::Mode::SawEsc:
                append_pending_or_flush(byte);
                if (byte == '[') {
                    stripper.mode = SgrStripperState::Mode::InCsi;
                    break;
                }

                // Not a CSI; treat as literal.
                flush_pending_as_literal();
                stripper.mode = SgrStripperState::Mode::Normal;
                break;

            case SgrStripperState::Mode::InCsi:
                append_pending_or_flush(byte);
                if ((byte >= '0' && byte <= '9') || byte == ';')
                    break;

                if (byte == 'm') {
                    // SGR sequence complete: drop it.
                    stripper.pending_length = 0;
                    stripper.mode = SgrStripperState::Mode::Normal;
                    break;
                }

                // Some other CSI: treat as literal.
                flush_pending_as_literal();
                stripper.mode = SgrStripperState::Mode::Normal;
                break;
            }
        }

        flush_out();
    };

    // Stdout capture is stripped for artifacts but tee'd unmodified to the terminal.
    if (capture.stdout_fd >= 0) {
        for (;;) {
            char buffer[4096];
            auto nread = ::read(capture.stdout_fd, buffer, sizeof(buffer));
            if (nread > 0) {
                if (capture.stdout_temp_file)
                    write_stripped_sgr_to_file(*capture.stdout_temp_file, ReadonlyBytes { buffer, static_cast<size_t>(nread) }, capture.stdout_sgr_stripper);
                if (capture.tee_to_terminal)
                    (void)::write(STDOUT_FILENO, buffer, static_cast<size_t>(nread));
                continue;
            }

            if (nread == 0) {
                stdout_reached_eof_or_error = true;
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            stdout_reached_eof_or_error = true;
            break;
        }
    }

    // Stderr capture is stripped for artifacts but tee'd unmodified to the terminal.
    if (capture.stderr_fd >= 0) {
        for (;;) {
            char buffer[4096];
            auto nread = ::read(capture.stderr_fd, buffer, sizeof(buffer));
            if (nread > 0) {
                if (capture.stderr_temp_file)
                    write_stripped_sgr_to_file(*capture.stderr_temp_file, ReadonlyBytes { buffer, static_cast<size_t>(nread) }, capture.stderr_sgr_stripper);
                if (capture.tee_to_terminal)
                    (void)::write(STDERR_FILENO, buffer, static_cast<size_t>(nread));
                continue;
            }

            if (nread == 0) {
                stderr_reached_eof_or_error = true;
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            stderr_reached_eof_or_error = true;
            break;
        }
    }

    if (stdout_reached_eof_or_error) {
        if (capture.stdout_notifier)
            capture.stdout_notifier->set_enabled(false);
        capture.stdout_notifier = nullptr;
        capture.stdout_fd = -1;
    }

    if (stderr_reached_eof_or_error) {
        if (capture.stderr_notifier)
            capture.stderr_notifier->set_enabled(false);
        capture.stderr_notifier = nullptr;
        capture.stderr_fd = -1;
    }
#else
    (void)capture;
#endif
}

ViewOutputCapture* output_capture_for_view(TestWebView& view)
{
    auto it = s_output_captures.find(&view);
    if (it == s_output_captures.end() || !it->value)
        return nullptr;
    return it->value.ptr();
}

ViewOutputCapture* ensure_output_capture_for_view(TestWebView& view)
{
    if (auto* existing = output_capture_for_view(view))
        return existing;

    auto pid = view.web_content_pid();
    bool tee_to_terminal = Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT;
    auto view_capture = create_output_capture_for_process(pid, tee_to_terminal);
    if (!view_capture)
        return nullptr;

    auto* result = view_capture.ptr();
    s_output_captures.set(&view, move(view_capture));
    return result;
}

void remove_output_capture_for_view(TestWebView& view)
{
    s_output_captures.remove(&view);
    if (auto it = s_extra_output_captures.find(&view); it != s_extra_output_captures.end()) {
        if (it->value.probe_timer)
            it->value.probe_timer->stop();
        s_extra_output_captures.remove(&view);
    }
}

static TestWebView* view_for_capture(ViewOutputCapture const& capture)
{
    for (auto& it : s_output_captures) {
        if (it.value && it.value.ptr() == &capture)
            return static_cast<TestWebView*>(const_cast<WebView::ViewImplementation*>(it.key));
    }
    return nullptr;
}

static bool has_capture_for_pid(Vector<ExtraProcessCapture> const& captures, pid_t pid)
{
    for (auto const& extra : captures) {
        if (extra.pid == pid)
            return true;
    }
    return false;
}

static ErrorOr<void> ensure_extra_output_captures_for_view(TestWebView& view, ExtraCaptureState& state)
{
    bool tee_to_terminal = Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT;

    if (auto pid = webaudio_worker_pid_for_view(view); pid.has_value()) {
        if (!has_capture_for_pid(state.captures, *pid)) {
            auto capture = create_output_capture_for_process(*pid, tee_to_terminal);
            if (capture) {
                auto prefix_base = ByteString::formatted("view{}-test{}-webaudio-worker", view.view_id(), state.test_index);
                TRY(begin_output_capture_for_process(*capture, state.tmp_dir, prefix_base));
                state.captures.append(ExtraProcessCapture {
                    .label = "webaudio worker"sv,
                    .file_prefix = "webaudio-worker"sv,
                    .pid = *pid,
                    .capture = move(capture),
                });
            }
        }
    }

    if (state.capture_audio_server) {
        if (auto pid = audio_server_pid(); pid.has_value()) {
            if (!has_capture_for_pid(state.captures, *pid)) {
                auto capture = create_output_capture_for_process(*pid, tee_to_terminal);
                if (capture) {
                    auto prefix_base = ByteString::formatted("view{}-test{}-audioserver", view.view_id(), state.test_index);
                    TRY(begin_output_capture_for_process(*capture, state.tmp_dir, prefix_base));
                    state.captures.append(ExtraProcessCapture {
                        .label = "audioserver"sv,
                        .file_prefix = "audioserver"sv,
                        .pid = *pid,
                        .capture = move(capture),
                    });
                }
            }
        }
    }

    return {};
}

ErrorOr<void> begin_output_capture_for_test(TestWebView& view, Test const& test)
{
    auto* capture = ensure_output_capture_for_view(view);
    if (!capture)
        return {};

    auto& app = Application::the();
    auto tmp_dir = LexicalPath::join(app.results_directory, ".tmp"sv).string();
    auto prefix_base = ByteString::formatted("view{}-test{}", view.view_id(), test.index);

    TRY(begin_output_capture_for_process(*capture, tmp_dir, prefix_base));
    ExtraCaptureState state;
    state.tmp_dir = tmp_dir;
    state.test_index = test.index;
    state.capture_audio_server = app.test_concurrency == 1 && view.view_id() == 0;
    s_extra_output_captures.set(&view, move(state));

    auto it = s_extra_output_captures.find(&view);
    if (it != s_extra_output_captures.end()) {
        TRY(ensure_extra_output_captures_for_view(view, it->value));
        if (!it->value.probe_timer) {
            it->value.probe_timer = Core::Timer::create_repeating(50, [view_ptr = &view] {
                auto state_it = s_extra_output_captures.find(view_ptr);
                if (state_it == s_extra_output_captures.end())
                    return;
                (void)ensure_extra_output_captures_for_view(*view_ptr, state_it->value);
            });
            it->value.probe_timer->start();
        }
    }

    return {};
}

ErrorOr<void> append_to_stderr_capture(ViewOutputCapture& capture, StringView text)
{
    drain_fds_to_temp_files(capture);
    if (capture.stderr_temp_file)
        TRY(capture.stderr_temp_file->write_until_depleted(text.bytes()));
    return {};
}

ErrorOr<void> finalize_output_for_test(Test const& test, TestResult result, ViewOutputCapture& capture, StringView base_path)
{
    drain_fds_to_temp_files(capture);

    auto* view = view_for_capture(capture);
    Vector<ExtraProcessCapture>* extra_captures = nullptr;
    ExtraCaptureState* extra_state = nullptr;
    if (view) {
        if (auto it = s_extra_output_captures.find(view); it != s_extra_output_captures.end()) {
            extra_state = &it->value;
            extra_captures = &it->value.captures;
        }
    }
    if (view && !extra_captures) {
        auto tmp_dir = LexicalPath::join(Application::the().results_directory, ".tmp"sv).string();
        ExtraCaptureState late_state;
        late_state.tmp_dir = tmp_dir;
        late_state.test_index = test.index;
        late_state.capture_audio_server = Application::the().test_concurrency == 1 && view->view_id() == 0;
        s_extra_output_captures.set(view, move(late_state));
        if (auto it = s_extra_output_captures.find(view); it != s_extra_output_captures.end()) {
            if (!ensure_extra_output_captures_for_view(*view, it->value).is_error()) {
                extra_state = &it->value;
                extra_captures = &it->value.captures;
            }
        }
    }
    if (extra_captures) {
        for (auto& extra : *extra_captures) {
            if (extra.capture)
                drain_fds_to_temp_files(*extra.capture);
        }
    }

    TRY(Core::Directory::create(LexicalPath::dirname(base_path), Core::Directory::CreateDirectories::Yes));

    if ((result == TestResult::Timeout || result == TestResult::Crashed) && !test.expectation_path.is_empty()) {
        auto expectation_file_or_error = Core::File::open(test.expectation_path, Core::File::OpenMode::Read);
        if (!expectation_file_or_error.is_error()) {
            auto expectation = TRY(expectation_file_or_error.value()->read_until_eof());
            auto expected_path = ByteString::formatted("{}.expected.txt", base_path);
            auto expected_file = TRY(Core::File::open(expected_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            TRY(expected_file->write_until_depleted(expectation));
        }
    }

    bool keep_logs = result == TestResult::Fail || result == TestResult::Timeout || result == TestResult::Crashed;

    capture.stdout_temp_file = nullptr;
    capture.stderr_temp_file = nullptr;

    if (keep_logs) {
        auto logging_path = ByteString::formatted("{}.logging.txt", base_path);
        auto logging_file = TRY(Core::File::open(logging_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));

        auto append_temp_file_contents = [&](ByteString const& temp_path) -> ErrorOr<void> {
            if (temp_path.is_empty())
                return {};
            auto input_or_error = Core::File::open(temp_path, Core::File::OpenMode::Read);
            if (input_or_error.is_error())
                return {};
            auto data = TRY(input_or_error.value()->read_until_eof());
            if (!data.is_empty()) {
                TRY(logging_file->write_until_depleted(data.bytes()));
                if (data[data.size() - 1] != '\n')
                    TRY(logging_file->write_until_depleted("\n"sv.bytes()));
            }
            return {};
        };

        TRY(logging_file->write_until_depleted("==== stdout ====\n"sv.bytes()));
        TRY(append_temp_file_contents(capture.stdout_temp_path));
        TRY(logging_file->write_until_depleted("==== stderr ====\n"sv.bytes()));
        TRY(append_temp_file_contents(capture.stderr_temp_path));

        if (extra_captures) {
            pid_t owner_pid = view ? view->web_content_pid() : -1;
            for (auto& extra : *extra_captures) {
                if (!extra.capture)
                    continue;
                auto stdout_header = ByteString::formatted("==== {} stdout (pid {}, webcontent pid {}) ====\n", extra.label, extra.pid, owner_pid);
                TRY(logging_file->write_until_depleted(stdout_header.bytes()));
                TRY(append_temp_file_contents(extra.capture->stdout_temp_path));
                auto stderr_header = ByteString::formatted("==== {} stderr (pid {}, webcontent pid {}) ====\n", extra.label, extra.pid, owner_pid);
                TRY(logging_file->write_until_depleted(stderr_header.bytes()));
                TRY(append_temp_file_contents(extra.capture->stderr_temp_path));
            }
        }
    }

    auto unlink_if_exists = [&](ByteString const& temp_path) -> ErrorOr<void> {
        if (temp_path.is_empty())
            return {};
        auto stat_or_error = Core::System::stat(temp_path);
        if (stat_or_error.is_error())
            return {};
        TRY(Core::System::unlink(temp_path));
        return {};
    };

    TRY(unlink_if_exists(capture.stdout_temp_path));
    TRY(unlink_if_exists(capture.stderr_temp_path));
    if (extra_captures) {
        for (auto& extra : *extra_captures) {
            if (!extra.capture)
                continue;
            TRY(unlink_if_exists(extra.capture->stdout_temp_path));
            TRY(unlink_if_exists(extra.capture->stderr_temp_path));
            extra.capture->stdout_temp_path = {};
            extra.capture->stderr_temp_path = {};
            extra.capture->stdout_temp_file = nullptr;
            extra.capture->stderr_temp_file = nullptr;
        }
    }

    if (extra_state && extra_state->probe_timer)
        extra_state->probe_timer->stop();
    if (view)
        s_extra_output_captures.remove(view);

    capture.stdout_temp_path = {};
    capture.stderr_temp_path = {};
    return {};
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
    builder.appendff("==== timeout diagnostics ====\n");
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

    // If we finished before the timeout fired, ensure the timer won't fire after we return.
    timeout_timer->stop();
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

    if (auto dump_path = Core::Environment::get("AUDIO_SERVER_STDERR_DUMP"sv); dump_path.has_value() && !dump_path->is_empty()) {
        stderr_builder.append("==== audioserver stderr ====\n"sv);
        auto file_or_error = Core::File::open(*dump_path, Core::File::OpenMode::Read);
        if (file_or_error.is_error()) {
            stderr_builder.appendff("(failed to open {}: {})\n", *dump_path, file_or_error.error());
            return;
        }

        auto contents_or_error = file_or_error.value()->read_until_eof();
        if (contents_or_error.is_error()) {
            stderr_builder.appendff("(failed to read {}: {})\n", *dump_path, contents_or_error.error());
            return;
        }

        auto const& contents = contents_or_error.value();
        constexpr size_t max_dump_bytes = 64uz * 1024uz;
        size_t bytes_to_append = min(contents.size(), max_dump_bytes);

        stderr_builder.append(StringView { reinterpret_cast<char const*>(contents.data()), bytes_to_append });
        if (contents.size() > max_dump_bytes)
            stderr_builder.append("\n(truncated)\n"sv);
    }
}

void append_timeout_backtraces_to_stderr(StringBuilder& stderr_builder, TestWebView& view, Test const& test, size_t view_id)
{
    append_diagnostics_header(stderr_builder, test, view_id, view.url().to_byte_string());
    append_backtrace_for_process(stderr_builder, "webcontent"sv, view.web_content_pid());
}

}
