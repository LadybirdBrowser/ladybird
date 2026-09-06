/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AssertionFailure.h>
#include <AK/ScopeGuard.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/ProcessManager.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static ByteString test_directory()
{
    return ByteString::formatted("{}/test-crash-report-{}", Core::StandardPaths::tempfile_directory(), getpid());
}

static Vector<ByteString> report_paths()
{
    Vector<ByteString> paths;
    Core::DirIterator iterator(test_directory(), Core::DirIterator::SkipDots);
    while (iterator.has_next())
        paths.append(iterator.next_full_path());
    return paths;
}

static void cleanup()
{
    if (FileSystem::exists(test_directory()))
        MUST(FileSystem::remove(test_directory(), FileSystem::RecursionMode::Allowed));
}

enum class CrashThread {
    Main,
    Worker,
};

static int crash_child(WebView::CrashReport& report, int signal, CrashThread thread = CrashThread::Main)
{
    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        struct rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);
        MUST(Core::CrashHandler::initialize(report.fd()));
        if (thread == CrashThread::Worker) {
            auto crash_worker = [](void* argument) -> void* {
                raise(*static_cast<int*>(argument));
                return nullptr;
            };
            pthread_t worker;
            VERIFY(pthread_create(&worker, nullptr, crash_worker, &signal) == 0);
            VERIFY(pthread_join(worker, nullptr) == 0);
        } else {
            raise(signal);
        }
        _exit(0);
    }
    int status;
    VERIFY(waitpid(pid, &status, 0) == pid);
    return status;
}

TEST_CASE(capture_native_crash_without_personal_data)
{
    cleanup();
    ScopeGuard guard = cleanup;
    for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP }) {
        auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
        auto status = crash_child(*report, signal);
        EXPECT(WIFSIGNALED(status));
        EXPECT_EQ(WTERMSIG(status), signal);
        MUST(report->save(status, test_directory()));
    }
    auto paths = report_paths();
    EXPECT_EQ(paths.size(), 6u);
    for (auto const& path : paths) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto contents = MUST(file->read_until_eof());
        StringView text { contents };
        EXPECT(text.contains("Captured signal:"sv));
        EXPECT(text.contains("Executable build ID:"sv));
        EXPECT(text.contains("#0 "sv));
        EXPECT(text.contains("#1 "sv));
        EXPECT(text.contains(" + 0x"sv));
        EXPECT(!text.contains(Core::StandardPaths::home_directory()));
        EXPECT(!text.contains(test_directory()));
        EXPECT(!text.contains("/Users/"sv));
        EXPECT(!text.contains("/home/"sv));
        EXPECT_EQ(MUST(file->stat()).st_mode & 0777u, 0600u);
    }
    EXPECT_EQ(MUST(Core::System::stat(test_directory())).st_mode & 0777u, 0700u);
}

TEST_CASE(worker_crash_preserves_signal_termination)
{
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    auto status = crash_child(*report, SIGABRT, CrashThread::Worker);
    EXPECT(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGABRT);
}

TEST_CASE(normal_exit_does_not_create_a_report)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    MUST(report->save(0, test_directory()));
    EXPECT(!FileSystem::exists(test_directory()));
}

TEST_CASE(missing_or_malformed_capture_produces_a_minimal_report)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    constexpr auto private_text = "https://private.example/secret user@example.com /Users/private"sv;
    VERIFY(pwrite(report->fd(), private_text.characters_without_null_termination(), private_text.length(), 0) == static_cast<ssize_t>(private_text.length()));
    MUST(report->save(SIGKILL, test_directory()));
    auto paths = report_paths();
    EXPECT_EQ(paths.size(), 1u);
    auto file = MUST(Core::File::open(paths[0], Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    StringView text { contents };
    EXPECT(text.contains("Termination signal: SIGKILL"sv));
    EXPECT(text.contains("Unavailable:"sv));
    EXPECT(!text.contains(private_text));
}

TEST_CASE(retention_is_bounded)
{
    cleanup();
    ScopeGuard guard = cleanup;
    MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto legacy_path = ByteString::formatted("{}/WebContent-legacy.txt", test_directory());
    auto legacy_file = MUST(Core::File::open(legacy_path, Core::File::OpenMode::Write));
    MUST(legacy_file->write_until_depleted("Legacy report\n"sv.bytes()));
    timespec legacy_times[2] { { 1, 0 }, { 1, 0 } };
    VERIFY(futimens(legacy_file->fd(), legacy_times) == 0);
    Vector<ByteString> created_paths { legacy_path };
    for (u32 i = 0; i < 25; ++i) {
        auto type = i % 2 ? WebView::ProcessType::RequestServer : WebView::ProcessType::WebContent;
        auto report = MUST(WebView::CrashReport::create(type));
        MUST(report->save(SIGKILL, test_directory()));
        auto previous_count = created_paths.size();
        for (auto const& path : report_paths()) {
            if (created_paths.contains_slow(path))
                continue;
            auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
            timespec times[2] { { 2 + i, 0 }, { 2 + i, 0 } };
            VERIFY(futimens(file->fd(), times) == 0);
            created_paths.append(path);
        }
        EXPECT_EQ(created_paths.size(), previous_count + 1);
    }
    auto retained_paths = report_paths();
    EXPECT_EQ(retained_paths.size(), 20u);
    for (auto const& path : created_paths.span().slice(6))
        EXPECT(retained_paths.contains_slow(path));
    EXPECT(!retained_paths.contains_slow(legacy_path));
}

TEST_CASE(invalid_frame_lengths_are_ignored)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    Core::CrashReportData::ReportHeader header;
    header.frame_count = 1;
    header.executable.size = 0xffffffff;
    header.assertion.kind = 1;
    header.assertion.length = 0xffffffff;
    Core::CrashReportData::ReportFrame frame;
    frame.binary.size = 0xffffffff;
    VERIFY(pwrite(report->fd(), &header, sizeof(header), 0) == sizeof(header));
    VERIFY(pwrite(report->fd(), &frame, sizeof(frame), sizeof(header)) == sizeof(frame));
    MUST(report->save(SIGSEGV, test_directory()));
    auto paths = report_paths();
    EXPECT_EQ(paths.size(), 1u);
    auto file = MUST(Core::File::open(paths[0], Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    EXPECT(StringView { contents }.contains("#0 unavailable"sv));
    EXPECT(!StringView { contents }.contains("Verification failed:"sv));
}

TEST_CASE(frames_beyond_the_captured_count_are_ignored)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    Core::CrashReportData::ReportHeader header;
    header.frame_count = 1;
    Core::CrashReportData::ReportFrame frame;
    VERIFY(pwrite(report->fd(), &header, sizeof(header), 0) == sizeof(header));
    VERIFY(pwrite(report->fd(), &frame, sizeof(frame), sizeof(header)) == sizeof(frame));
    VERIFY(pwrite(report->fd(), &frame, sizeof(frame), sizeof(header) + sizeof(frame)) == sizeof(frame));
    MUST(report->save(SIGSEGV, test_directory()));
    auto file = MUST(Core::File::open(report_paths()[0], Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    EXPECT(StringView { contents }.contains("#0 unavailable"sv));
    EXPECT(!StringView { contents }.contains("#1"sv));
}

[[gnu::noinline]] static int exhaust_stack(size_t depth)
{
    if (depth == NumericLimits<size_t>::max())
        return 0;
    u8 volatile buffer[4096];
    buffer[depth % sizeof(buffer)] = static_cast<u8>(depth);
    auto result = exhaust_stack(depth + 1);
    return result + buffer[depth % sizeof(buffer)];
}

TEST_CASE(main_thread_stack_overflow)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        struct rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);
        MUST(Core::CrashHandler::initialize(report->fd()));
        _exit(exhaust_stack(0));
    }
    int status;
    VERIFY(waitpid(pid, &status, 0) == pid);
    EXPECT(WIFSIGNALED(status));
    MUST(report->save(status, test_directory()));
    auto paths = report_paths();
    EXPECT_EQ(paths.size(), 1u);
    auto file = MUST(Core::File::open(paths[0], Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    EXPECT(StringView { contents }.contains("Captured signal:"sv));
    EXPECT(StringView { contents }.contains("#0 "sv));
}

TEST_CASE(helper_process_names)
{
    cleanup();
    ScopeGuard guard = cleanup;
    for (auto type : { WebView::ProcessType::WebContent, WebView::ProcessType::WebWorker, WebView::ProcessType::RequestServer, WebView::ProcessType::ImageDecoder, WebView::ProcessType::Compositor, WebView::ProcessType::WasmCompiler }) {
        auto report = MUST(WebView::CrashReport::create(type));
        MUST(report->save(SIGKILL, test_directory()));
    }
    auto paths = report_paths();
    EXPECT_EQ(paths.size(), 6u);
    for (auto const& path : paths) {
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        auto contents = MUST(file->read_until_eof());
        auto basename = LexicalPath::basename(path);
        auto name = basename.substring_view(21).split_view('-')[0];
        EXPECT(StringView { contents }.contains(ByteString::formatted("Process: {}\n", name)));
    }
}

static ByteString assertion_report(AK::AssertionFailureKind kind, char const* message, ByteString* terminal_output = nullptr)
{
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    int stderr_pipe[2];
    if (terminal_output)
        VERIFY(pipe(stderr_pipe) == 0);
    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        if (terminal_output) {
            close(stderr_pipe[0]);
            VERIFY(dup2(stderr_pipe[1], STDERR_FILENO) == STDERR_FILENO);
            close(stderr_pipe[1]);
        }
        struct rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);
        MUST(Core::CrashHandler::initialize(report->fd()));
        if (kind == AK::AssertionFailureKind::Verification)
            ak_verification_failed(message);
        ak_assertion_failed(message);
    }
    if (terminal_output) {
        close(stderr_pipe[1]);
        auto terminal = MUST(Core::File::adopt_fd(stderr_pipe[0], Core::File::OpenMode::Read));
        auto contents = MUST(terminal->read_until_eof());
        *terminal_output = ByteString { StringView { contents } };
    }
    int status;
    VERIFY(waitpid(pid, &status, 0) == pid);
    EXPECT(WIFSIGNALED(status));
    MUST(report->save(status, test_directory()));
    auto paths = report_paths();
    VERIFY(paths.size() == 1);
    auto file = MUST(Core::File::open(paths[0], Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());
    return ByteString { StringView { contents } };
}

TEST_CASE(assertion_text_and_source_location)
{
    cleanup();
    ScopeGuard guard = cleanup;
    ByteString terminal;
    auto text = assertion_report(AK::AssertionFailureKind::Verification, "value != 0 at " __FILE__ ":123", &terminal);
    EXPECT(text.contains("Verification failed: value != 0 at Tests/LibWebView/TestCrashReport.cpp:123\n"sv));
    EXPECT(text.contains("Captured signal:"sv));
    EXPECT(text.contains("#0 "sv));
    EXPECT(!text.contains(Core::StandardPaths::home_directory()));
#if defined(TEST_HAS_CPPTRACE)
    EXPECT(text.contains("assertion_report"sv));
    EXPECT_EQ(text.count("(inlined)"sv), terminal.count("(inlined)"sv));
    EXPECT_EQ(text.count("\n#"sv), terminal.count("\n#"sv));
    auto stack = text.substring_view(text.find("Native stack"sv).value());
    EXPECT(stack.contains(" at Tests/LibWebView/TestCrashReport.cpp:"sv));
    EXPECT(!stack.contains("ak_trap"sv));
#endif
}

TEST_CASE(assertion_locations_outside_the_checkout)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto text = assertion_report(AK::AssertionFailureKind::Assertion, "value != 0 at /Users/private/build/Assertion.cpp:42");
    EXPECT(text.contains("Assertion failed: value != 0 at Assertion.cpp:42\n"sv));
    EXPECT(!text.contains("/Users/private"sv));
}

TEST_CASE(resolved_backtrace_descriptions_are_private_and_bounded)
{
    EXPECT(Core::CrashReportData::sanitize_backtrace_symbol("bad\nsymbol"sv).is_empty());
    EXPECT(Core::CrashReportData::sanitize_backtrace_symbol("C:\\private\\file"sv).is_empty());
    Core::CrashReportData::ReportFrame frame;
    AK::AssertionBacktraceFrame resolved {
        0, "private_function"sv, "/home/private/checkout/Tests/Example.cpp"sv, 42, 7, true
    };
    Core::CrashReportData::describe_backtrace_frame(frame, resolved);
    EXPECT_EQ((StringView { frame.description.data(), frame.description_length }), "(inlined) private_function at Tests/Example.cpp:42:7"sv);

    frame = {};
    resolved.symbol = "generated</home/private/source.cpp>"sv;
    resolved.filename = "/home/private/external/Example.cpp"sv;
    resolved.is_inline = false;
    Core::CrashReportData::describe_backtrace_frame(frame, resolved);
    EXPECT_EQ((StringView { frame.description.data(), frame.description_length }), " at Example.cpp:42:7"sv);

    frame = {};
    auto long_symbol = ByteString::repeated('x', frame.description.size());
    resolved.symbol = long_symbol;
    Core::CrashReportData::describe_backtrace_frame(frame, resolved);
    EXPECT_EQ(frame.description_length, frame.description.size());
}

TEST_CASE(long_assertion_text_is_bounded)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto message = ByteString::repeated('x', 4096);
    auto text = assertion_report(AK::AssertionFailureKind::Verification, message.characters());
    EXPECT(text.contains(" [truncated]\n"sv));
    EXPECT(!text.contains(message));
    EXPECT(text.contains("#0 "sv));
}

TEST_CASE(oversized_assertion_location_is_omitted)
{
    cleanup();
    ScopeGuard guard = cleanup;
    auto message = ByteString::formatted("value != 0 at /Users/private/{}:42", ByteString::repeated('x', 8192));
    auto text = assertion_report(AK::AssertionFailureKind::Verification, message.characters());
    EXPECT(text.contains("Verification failed: value != 0 at [location unavailable] [truncated]\n"sv));
    EXPECT(!text.contains("/Users/private"sv));
}
