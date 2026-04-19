/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TestRunCapture.h"
#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/LexicalPath.h>
#include <LibCore/System.h>

namespace TestWeb {

static ByteString format_elapsed_time(UnixDateTime run_start_time);
static void setup_capture_notifier(RefPtr<Core::Notifier>& notifier, int fd, bool drain_available, Function<void(StringView)> on_output);
static bool drain_capture_output(int fd, bool drain_available, Function<void(StringView)> const& on_output);

TestRunCapture::TestRunCapture()
    : m_run_started_at(UnixDateTime::now())
{
    ByteString helper_logs_path = LexicalPath::join(Application::the().results_directory, "helper-process-logs.html"sv).string();
    m_helper_output = CaptureFile { move(helper_logs_path) };

    auto& process_manager = WebView::Application::process_manager();
    process_manager.on_process_added = [this](WebView::Process& process) {
        setup_output_capture_for_helper_process(process);
    };
    m_previous_on_process_exited = move(process_manager.on_process_exited);
    process_manager.on_process_exited = [this](WebView::Process&& process) {
        consume_helper_capture(process.pid());
        m_previous_on_process_exited(move(process));
    };

    process_manager.for_each_process([this](WebView::Process& process) {
        setup_output_capture_for_helper_process(process);
    });

#ifndef AK_OS_WINDOWS
    auto dup_stderr_fd = MUST(Core::System::dup(STDERR_FILENO));
    auto pipe_fds = MUST(Core::System::pipe2(O_CLOEXEC));
    auto read_fd = pipe_fds[0];
    auto write_fd = pipe_fds[1];
    auto current_flags = Core::System::fcntl(read_fd, F_GETFL);
    if (!current_flags.is_error())
        (void)Core::System::fcntl(read_fd, F_SETFL, current_flags.value() | O_NONBLOCK);

    (void)fflush(stderr);
    if (::dup2(write_fd, STDERR_FILENO) < 0) {
        MUST(Core::System::close(read_fd));
        MUST(Core::System::close(write_fd));
        MUST(Core::System::close(dup_stderr_fd));
        return;
    }
    MUST(Core::System::close(write_fd));

    m_stderr_capture.original_fd = dup_stderr_fd;
    m_stderr_capture.reader = MUST(Core::File::adopt_fd(read_fd, Core::File::OpenMode::Read));
    int browser_pid = Core::System::getpid();
    setup_capture_notifier(m_stderr_capture.notifier, m_stderr_capture.reader->fd(), true, [this, browser_pid](StringView message) {
        log_helper_message({ WebView::ProcessType::Browser, browser_pid }, m_stderr_capture.original_fd.value_or(STDERR_FILENO), message);
    });
#endif
}

TestRunCapture::~TestRunCapture()
{
    restore_stderr();
    WebView::Application::process_manager().on_process_added = {};
    WebView::Application::process_manager().on_process_exited = move(m_previous_on_process_exited);
}

TestRunCapture::ViewOutputCapture* TestRunCapture::output_capture_for_view(TestWebView const& view)
{
    auto capture = m_test_output_captures.get(&view);
    if (!capture.has_value())
        return nullptr;
    return capture.value();
}

void TestRunCapture::log_helper_message(HelperOutputSource source, int tee_fd, StringView message)
{
    if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
        (void)Core::System::write(tee_fd, message.bytes());

    bool const should_append_header = !m_last_helper_source.has_value()
        || m_last_helper_source->type != source.type
        || m_last_helper_source->pid != source.pid;

    if (should_append_header) {
        if (m_last_helper_source.has_value())
            m_helper_output.write("\n"sv);

        auto elapsed_time = format_elapsed_time(m_run_started_at);
        ByteString header = ByteString::formatted("===== {} ({}) at {} =====\n", WebView::process_name_from_type(source.type), source.pid, elapsed_time);
        m_helper_output.write(header);
        m_last_helper_source = source;
    }

    m_helper_output.write(message);
}

void TestRunCapture::setup_output_capture_for_view(TestWebView& view, ViewOutputCapture& view_capture)
{
    auto process = Application::the().find_process(view.web_content_pid());
    if (!process.has_value())
        return;

    auto& output_capture = process->output_capture();
    if (!output_capture.stdout_file && !output_capture.stderr_file)
        return;

    if (output_capture.stdout_file) {
        setup_capture_notifier(view_capture.stdout_notifier, output_capture.stdout_file->fd(), false, [&view_capture](StringView message) {
            if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                (void)Core::System::write(STDOUT_FILENO, message.bytes());
            view_capture.output.write(message);
        });
    }

    if (output_capture.stderr_file) {
        setup_capture_notifier(view_capture.stderr_notifier, output_capture.stderr_file->fd(), false, [this, &view_capture](StringView message) {
            if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                (void)Core::System::write(m_stderr_capture.original_fd.value_or(STDERR_FILENO), message.bytes());
            view_capture.output.write(message);
        });
    }
}

void TestRunCapture::begin_test_output_capture(TestWebView& view, Test const& test)
{
    destroy_view_capture_of(view);

    auto view_capture = make<ViewOutputCapture>();
    ByteString output_path = ByteString::formatted("{}.logs.html", LexicalPath::join(Application::the().results_directory, test.safe_relative_path).string());
    view_capture->output = CaptureFile { move(output_path) };

    setup_output_capture_for_view(view, *view_capture);
    m_test_output_captures.set(&view, move(view_capture));
}

void TestRunCapture::rebind_test_output_capture(TestWebView& view)
{
    auto* capture = output_capture_for_view(view);
    if (!capture)
        return;

    setup_output_capture_for_view(view, *capture);
}

void TestRunCapture::write_test_output(TestWebView const& view)
{
    auto* capture = output_capture_for_view(view);
    if (!capture)
        return;

    (void)capture->output.transfer_to_output_file();
    destroy_view_capture_of(view);
}

bool TestRunCapture::write_helper_process_output()
{
    restore_stderr();
    return m_helper_output.transfer_to_output_file().value_or(false);
}

void TestRunCapture::restore_stderr()
{
    if (!m_stderr_capture.original_fd.has_value() || !m_stderr_capture.reader)
        return;

    (void)fflush(stderr);
    int browser_pid = Core::System::getpid();
    (void)drain_capture_output(m_stderr_capture.reader->fd(), true, [this, browser_pid](StringView message) {
        log_helper_message({ WebView::ProcessType::Browser, browser_pid }, m_stderr_capture.original_fd.value_or(STDERR_FILENO), message);
    });
    auto original_stderr_fd = m_stderr_capture.original_fd.release_value();

#ifndef AK_OS_WINDOWS
    (void)::dup2(original_stderr_fd, STDERR_FILENO);
#endif
    MUST(Core::System::close(original_stderr_fd));

    if (m_stderr_capture.notifier)
        m_stderr_capture.notifier->close();
    m_stderr_capture.notifier = nullptr;
    m_stderr_capture.reader = nullptr;
}

void TestRunCapture::setup_output_capture_for_helper_process(WebView::Process& process)
{
    if (process.type() == WebView::ProcessType::Browser || process.type() == WebView::ProcessType::WebContent)
        return;

    auto const pid = process.pid();
    if (m_helper_output_captures.contains(pid))
        return;

    auto& output_capture = process.output_capture();
    if (!output_capture.stdout_file && !output_capture.stderr_file)
        return;

    auto helper_capture = make<HelperOutputCapture>();
    helper_capture->type = process.type();
    helper_capture->pid = pid;
    auto* helper_capture_ptr = helper_capture.ptr();

    if (output_capture.stdout_file) {
        helper_capture->stdout_reader = move(output_capture.stdout_file);
        setup_capture_notifier(helper_capture->stdout_notifier, helper_capture->stdout_reader->fd(), false, [this, capture = helper_capture_ptr](StringView message) {
            log_helper_message({ capture->type, capture->pid }, STDOUT_FILENO, message);
        });
    }
    if (output_capture.stderr_file) {
        helper_capture->stderr_reader = move(output_capture.stderr_file);
        setup_capture_notifier(helper_capture->stderr_notifier, helper_capture->stderr_reader->fd(), false, [this, capture = helper_capture_ptr](StringView message) {
            log_helper_message(
                { capture->type, capture->pid },
                m_stderr_capture.original_fd.value_or(STDERR_FILENO),
                message);
        });
    }
    m_helper_output_captures.set(pid, move(helper_capture));
}

static ByteString format_elapsed_time(UnixDateTime run_start_time)
{
    auto elapsed_milliseconds = AK::max((UnixDateTime::now() - run_start_time).to_truncated_milliseconds(), 0);
    i64 total_seconds = elapsed_milliseconds / 1000;
    i64 centiseconds = (elapsed_milliseconds / 10) % 100;
    i64 minutes = total_seconds / 60;
    i64 seconds = total_seconds % 60;
    if (minutes > 0)
        return ByteString::formatted("{}:{:02}.{:02}s", minutes, seconds, centiseconds);
    return ByteString::formatted("{}.{:02}s", seconds, centiseconds);
}

static void setup_capture_notifier(RefPtr<Core::Notifier>& notifier, int fd, bool drain_available, Function<void(StringView)> on_output)
{
    notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
    auto* notifier_slot = &notifier;
    notifier->on_activation = [fd, drain_available, on_output = move(on_output), notifier_slot]() mutable {
        bool const reached_eof = drain_capture_output(fd, drain_available, on_output);
        if (reached_eof && *notifier_slot)
            (*notifier_slot)->set_enabled(false);
    };
}

static bool drain_capture_output(int fd, bool drain_available, Function<void(StringView)> const& on_output)
{
    for (;;) {
        char buffer[4096];
        auto nread = Core::System::read(fd, Bytes { buffer, sizeof(buffer) });
        if (nread.is_error())
            return false;

        if (nread.value() > 0) {
            on_output(StringView { buffer, nread.value() });
            if (!drain_available)
                return false;

            continue;
        }

        return nread.value() == 0;
    }
}

void TestRunCapture::consume_helper_capture(pid_t pid)
{
    auto capture = m_helper_output_captures.take(pid);
    if (!capture.has_value())
        return;

    auto helper_capture = capture.release_value();

    helper_capture->stdout_notifier->set_enabled(false);
    helper_capture->stderr_notifier->set_enabled(false);

    if (helper_capture->stdout_reader) {
        (void)drain_capture_output(helper_capture->stdout_reader->fd(), true, [this, type = helper_capture->type, pid = helper_capture->pid](StringView message) {
            log_helper_message({ type, pid }, STDOUT_FILENO, message);
        });
    }
    if (helper_capture->stderr_reader) {
        (void)drain_capture_output(helper_capture->stderr_reader->fd(), true, [this, type = helper_capture->type, pid = helper_capture->pid](StringView message) {
            log_helper_message({ type, pid }, m_stderr_capture.original_fd.value_or(STDERR_FILENO), message);
        });
    }
    helper_capture->stdout_notifier->close();
    helper_capture->stderr_notifier->close();
}

void TestRunCapture::destroy_view_capture_of(TestWebView const& view)
{
    auto capture = m_test_output_captures.take(&view);
    if (!capture.has_value())
        return;

    auto view_capture = capture.release_value();
    if (view_capture->stdout_notifier)
        view_capture->stdout_notifier->close();
    if (view_capture->stderr_notifier)
        view_capture->stderr_notifier->close();
}

}
