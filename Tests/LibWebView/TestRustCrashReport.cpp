/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <AK/ScopeGuard.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/DirIterator.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReport.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" void test_rust_panic(u8);

enum class PanicMode : u8 {
    Fatal,
    Recovered,
    LongMessage,
    WorkerRecovered,
    RecoveredThenFatal,
    WorkerFatal,
    NonStringPayload,
};

enum class NativeCrash {
    No,
    Yes,
};

static ByteString run_panic(PanicMode mode, NativeCrash native_crash = NativeCrash::No)
{
    auto directory = ByteString::formatted("{}/test-rust-crash-{}", Core::StandardPaths::tempfile_directory(), getpid());
    ScopeGuard cleanup = [&] {
        if (FileSystem::exists(directory))
            MUST(FileSystem::remove(directory, FileSystem::RecursionMode::Allowed));
    };
    auto report = MUST(WebView::CrashReport::create(WebView::ProcessType::WebContent));
    auto pid = fork();
    VERIFY(pid >= 0);
    if (pid == 0) {
        struct rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);
        MUST(Core::CrashHandler::initialize(report->fd()));
        test_rust_panic(to_underlying(mode));
        if (native_crash == NativeCrash::Yes)
            raise(SIGSEGV);
        _exit(0);
    }
    int status;
    VERIFY(waitpid(pid, &status, 0) == pid);
    MUST(report->save(status, directory));
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        EXPECT(!FileSystem::exists(directory));
        return {};
    }
    EXPECT(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), native_crash == NativeCrash::Yes ? SIGSEGV : SIGABRT);
    Core::DirIterator iterator(directory, Core::DirIterator::SkipDots);
    VERIFY(iterator.has_next());
    auto file = MUST(Core::File::open(iterator.next_full_path(), Core::File::OpenMode::Read));
    auto bytes = MUST(file->read_until_eof());
    auto text = ByteString { StringView { bytes } };
    EXPECT(!text.contains(Core::StandardPaths::home_directory()));
    return text;
}

TEST_CASE(rust_panic_message_and_source_location)
{
    auto text = run_panic(PanicMode::Fatal);
    EXPECT(text.contains("Rust panic: expected Rust panic at Tests/LibWebView/RustCrashReport.rs:"sv));
    EXPECT(text.contains("Captured signal: SIGABRT\nCaptured signal number: 6\n"sv));
}

TEST_CASE(long_rust_panic_is_bounded)
{
    auto text = run_panic(PanicMode::LongMessage);
    EXPECT(text.contains("Rust panic: "sv));
    EXPECT(text.contains("[truncated]"sv));
    EXPECT(!text.contains(ByteString::repeated('x', 10000)));
}

TEST_CASE(worker_rust_panic_is_reported)
{
    EXPECT(run_panic(PanicMode::WorkerFatal).contains("Rust panic: expected Rust panic"sv));
}

TEST_CASE(non_string_rust_panic_is_reported)
{
    EXPECT(run_panic(PanicMode::NonStringPayload).contains("Rust panic: non-string panic payload"sv));
}

TEST_CASE(rust_symbols_are_demangled)
{
    EXPECT_EQ(demangle("_RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse"sv), "liburl_rust::url::parser::basic_parse"sv);
    EXPECT_EQ(demangle("__RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse"sv), "liburl_rust::url::parser::basic_parse"sv);
    EXPECT_EQ(demangle("_RINvMs_NtNtCsgKdBqEBFaNM_11liburl_rust3url5inputNtB5_8UrlInput10find_asciiNCNvNtB7_6parser16preprocess_input0EB9_"sv),
        "<liburl_rust::url::input::UrlInput>::find_ascii::<liburl_rust::url::parser::preprocess_input::{closure#0}>"sv);
    EXPECT_EQ(demangle("_RNvC_invalid"sv), "_RNvC_invalid"sv);
}

TEST_CASE(rust_symbols_from_symbolizers_are_demangled)
{
    // DbgHelp strips the leading underscore and may append a parameter list.
    EXPECT_EQ(demangle("RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse"sv), "liburl_rust::url::parser::basic_parse"sv);
    EXPECT_EQ(demangle_rust_symbol("RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse()"sv), "liburl_rust::url::parser::basic_parse()"sv);

    // Mach-O symbol tables add an extra underscore, and cpptrace appends the offset.
    EXPECT_EQ(demangle_rust_symbol("__RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse + 51"sv), "liburl_rust::url::parser::basic_parse + 51"sv);
    EXPECT(!demangle_rust_symbol("liburl_rust::url::parser::basic_parse"sv).has_value());
    EXPECT_EQ(demangle_backtrace_symbols_line("./TestRustCrashReport(_RNvNtNtCsgKdBqEBFaNM_11liburl_rust3url6parser11basic_parse+0x1a) [0x55d0c1a2]"sv),
        "./TestRustCrashReport(liburl_rust::url::parser::basic_parse+0x1a) [0x55d0c1a2]"sv);
}

#if defined(TEST_RUST_UNWIND)
TEST_CASE(recovered_rust_panic_does_not_create_a_report)
{
    EXPECT(run_panic(PanicMode::Recovered).is_empty());
}

TEST_CASE(recovered_rust_panic_does_not_contaminate_native_crash)
{
    auto text = run_panic(PanicMode::Recovered, NativeCrash::Yes);
    EXPECT(!text.contains("Rust panic:"sv));
    EXPECT(!text.contains("recovered Rust panic"sv));
}

TEST_CASE(worker_panic_does_not_contaminate_native_crash)
{
    auto text = run_panic(PanicMode::WorkerRecovered, NativeCrash::Yes);
    EXPECT(!text.contains("Rust panic:"sv));
}

TEST_CASE(fatal_rust_panic_replaces_recovered_panic)
{
    auto text = run_panic(PanicMode::RecoveredThenFatal);
    EXPECT(text.contains("Rust panic: expected Rust panic"sv));
    EXPECT(!text.contains("recovered Rust panic"sv));
}

#    if defined(TEST_HAS_CPPTRACE)
TEST_CASE(rust_frames_are_demangled)
{
    auto text = run_panic(PanicMode::Fatal);
    auto stack = text.substring_view(text.find("Native stack"sv).value());
    EXPECT(stack.contains(" rust_crash_report_unwind::rust_panic::abort_on_panic::<"sv));
    EXPECT(!stack.contains(" _R"sv));
    EXPECT(!stack.contains(" __R"sv));
}
#    endif
#endif
