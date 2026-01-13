/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/ByteBuffer.h>
#include <AK/Enumerate.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Notifier.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Process.h>
#include <LibWebView/Utilities.h>

#ifndef AK_OS_WINDOWS
#    include <sys/ioctl.h>
#endif

namespace TestWeb {

// Terminal display state
static size_t s_terminal_width = 80;
static bool s_is_tty = false;

static void update_terminal_size()
{
#ifndef AK_OS_WINDOWS
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        s_terminal_width = ws.ws_col > 0 ? ws.ws_col : 80;
#endif
}

struct ViewDisplayState {
    pid_t pid { 0 };
    ByteString test_name;
    UnixDateTime start_time;
    bool active { false };
};

static Vector<ViewDisplayState> s_view_display_states;
static RefPtr<Core::Timer> s_display_timer;

static size_t s_live_display_lines = 0;
static size_t s_total_tests = 0;
static size_t s_completed_tests = 0;
static size_t s_pass_count = 0;
static size_t s_fail_count = 0;
static size_t s_timeout_count = 0;
static size_t s_crashed_count = 0;
static size_t s_skipped_count = 0;

// Deferred warning system - buffers warnings during live display mode
static Vector<ByteString> s_deferred_warnings;

void add_deferred_warning(ByteString message)
{
    if (s_live_display_lines > 0)
        s_deferred_warnings.append(move(message));
    else
        warnln("{}", message);
}

void print_deferred_warnings()
{
    for (auto const& warning : s_deferred_warnings)
        warnln("{}", warning);
    s_deferred_warnings.clear();
}

static void render_live_display()
{
    if (!s_is_tty || s_live_display_lines == 0)
        return;

    auto now = UnixDateTime::now();

    // Build everything into one buffer
    StringBuilder output;

    // Move up N lines using individual commands (more compatible)
    for (size_t i = 0; i < s_live_display_lines; ++i)
        output.append("\033[A"sv);
    output.append("\r"sv);

    // Print test status lines (not counting empty lines, status counts, and progress bar)
    size_t num_view_lines = s_live_display_lines - 4;
    for (size_t i = 0; i < num_view_lines; ++i) {
        output.append("\033[2K"sv); // Clear line

        if (i < s_view_display_states.size()) {
            auto const& state = s_view_display_states[i];
            if (state.active && state.pid > 0) {
                auto duration = (now - state.start_time).to_seconds();
                // Format: ⏺ pid (Xs): name
                auto prefix = ByteString::formatted("\033[33m⏺\033[0m {} ({}s): ", state.pid, duration);
                // Note: prefix contains ANSI codes, so visible length is different
                size_t prefix_visible_len = ByteString::formatted("⏺ {} ({}s): ", state.pid, duration).length();
                size_t avail = s_terminal_width > prefix_visible_len ? s_terminal_width - prefix_visible_len : 10;

                ByteString name = state.test_name;
                if (name.length() > avail && avail > 3)
                    name = ByteString::formatted("...{}", name.substring_view(name.length() - avail + 3));

                output.appendff("{}{}", prefix, name);
            } else {
                output.append("\033[90m⏺ (idle)\033[0m"sv);
            }
        }
        output.append("\n"sv);
    }

    // Empty line
    output.append("\033[2K\n"sv);

    // Status counts line (bold colored labels, plain numbers)
    output.append("\033[2K"sv);
    output.appendff("\033[1;32mPass:\033[0m {}, ", s_pass_count);
    output.appendff("\033[1;31mFail:\033[0m {}, ", s_fail_count);
    output.appendff("\033[1;90mSkipped:\033[0m {}, ", s_skipped_count);
    output.appendff("\033[1;33mTimeout:\033[0m {}, ", s_timeout_count);
    output.appendff("\033[1;35mCrashed:\033[0m {}", s_crashed_count);
    output.append("\n"sv);

    // Empty line
    output.append("\033[2K\n"sv);

    // Print progress bar
    output.append("\033[2K"sv);
    if (s_total_tests > 0) {
        size_t completed = s_completed_tests;
        size_t total = s_total_tests;

        // Calculate progress bar width (leave room for "completed/total []")
        auto counter = ByteString::formatted("{}/{} ", completed, total);
        size_t bar_width = s_terminal_width > counter.length() + 3 ? s_terminal_width - counter.length() - 3 : 20;

        size_t filled = total > 0 ? (completed * bar_width) / total : 0;
        size_t empty = bar_width - filled;

        output.append(counter);
        output.append("\033[32m["sv); // Green color
        for (size_t j = 0; j < filled; ++j)
            output.append("█"sv);
        if (empty > 0 && filled < bar_width) {
            output.append("\033[33m▓\033[0m\033[90m"sv); // Yellow current position, then dim
            for (size_t j = 1; j < empty; ++j)
                output.append("░"sv);
        }
        output.append("\033[32m]\033[0m"sv);
    }
    output.append("\n"sv);

    out("{}", output.string_view());
    (void)fflush(stdout);
}

static RefPtr<Core::Promise<Empty>> s_all_tests_complete;
static Vector<ByteString> s_skipped_tests;
static HashMap<WebView::ViewImplementation const*, Test*> s_test_by_view;

struct ViewOutputCapture {
    StringBuilder stdout_buffer;
    StringBuilder stderr_buffer;
    RefPtr<Core::Notifier> stdout_notifier;
    RefPtr<Core::Notifier> stderr_notifier;
};

static HashMap<WebView::ViewImplementation const*, OwnPtr<ViewOutputCapture>> s_output_captures;

static void setup_output_capture_for_view(TestWebView& view)
{
    auto pid = view.web_content_pid();
    auto process = Application::the().find_process(pid);
    if (!process.has_value())
        return;

    auto& output_capture = process->output_capture();
    if (!output_capture.stdout_file && !output_capture.stderr_file)
        return;

    auto view_capture = make<ViewOutputCapture>();

    if (output_capture.stdout_file) {
        auto fd = output_capture.stdout_file->fd();
        view_capture->stdout_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        view_capture->stdout_notifier->on_activation = [fd, &capture = *view_capture]() {
            char buffer[4096];
            auto nread = read(fd, buffer, sizeof(buffer));
            if (nread > 0)
                capture.stdout_buffer.append(StringView { buffer, static_cast<size_t>(nread) });
            else
                capture.stdout_notifier->set_enabled(false);
        };
    }

    if (output_capture.stderr_file) {
        auto fd = output_capture.stderr_file->fd();
        view_capture->stderr_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        view_capture->stderr_notifier->on_activation = [fd, &capture = *view_capture]() {
            char buffer[4096];
            auto nread = read(fd, buffer, sizeof(buffer));
            if (nread > 0)
                capture.stderr_buffer.append(StringView { buffer, static_cast<size_t>(nread) });
            else
                capture.stderr_notifier->set_enabled(false);
        };
    }

    s_output_captures.set(&view, move(view_capture));
}

static ErrorOr<void> write_output_for_test(Test const& test, ViewOutputCapture& capture)
{
    auto& app = Application::the();
    if (app.results_directory.is_empty())
        return {};

    // Create the directory structure for this test's output
    auto output_dir = LexicalPath::join(app.results_directory, LexicalPath::dirname(test.relative_path)).string();
    TRY(Core::Directory::create(output_dir, Core::Directory::CreateDirectories::Yes));

    auto base_path = LexicalPath::join(app.results_directory, test.relative_path).string();

    // Write stdout if not empty
    if (!capture.stdout_buffer.is_empty()) {
        auto stdout_path = ByteString::formatted("{}.stdout.txt", base_path);
        auto file = TRY(Core::File::open(stdout_path, Core::File::OpenMode::Write));
        TRY(file->write_until_depleted(capture.stdout_buffer.string_view().bytes()));
    }

    // Write stderr if not empty
    if (!capture.stderr_buffer.is_empty()) {
        auto stderr_path = ByteString::formatted("{}.stderr.txt", base_path);
        auto file = TRY(Core::File::open(stderr_path, Core::File::OpenMode::Write));
        TRY(file->write_until_depleted(capture.stderr_buffer.string_view().bytes()));
    }

    // Clear buffers for next test
    capture.stdout_buffer.clear();
    capture.stderr_buffer.clear();

    return {};
}

static constexpr StringView test_result_to_string(TestResult result)
{
    switch (result) {
    case TestResult::Pass:
        return "Pass"sv;
    case TestResult::Fail:
        return "Fail"sv;
    case TestResult::Skipped:
        return "Skipped"sv;
    case TestResult::Timeout:
        return "Timeout"sv;
    case TestResult::Crashed:
        return "Crashed"sv;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<void> load_test_config(StringView test_root_path)
{
    auto config_path = LexicalPath::join(test_root_path, "TestConfig.ini"sv);
    auto config_or_error = Core::ConfigFile::open(config_path.string());

    if (config_or_error.is_error()) {
        if (config_or_error.error().code() == ENOENT)
            return {};
        warnln("Unable to open test config {}", config_path);
        return config_or_error.release_error();
    }

    auto config = config_or_error.release_value();
    for (auto const& group : config->groups()) {
        if (group == "Skipped"sv) {
            for (auto& key : config->keys(group))
                s_skipped_tests.append(TRY(FileSystem::real_path(LexicalPath::join(test_root_path, key).string())));
        } else {
            warnln("Unknown group '{}' in config {}", group, config_path);
        }
    }

    return {};
}

static bool is_valid_test_name(StringView test_name)
{
    auto valid_test_file_suffixes = { ".htm"sv, ".html"sv, ".svg"sv, ".xhtml"sv, ".xht"sv };
    return AK::any_of(valid_test_file_suffixes, [&](auto suffix) { return test_name.ends_with(suffix); });
}

static ErrorOr<void> collect_dump_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail, TestMode mode)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);

    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_dump_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name), mode));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.txt", path, trail, LexicalPath::title(name));
        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ mode, input_path, move(expectation_path), move(relative_path) });
    }

    return {};
}

static ErrorOr<void> collect_ref_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_ref_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Ref, input_path, {}, move(relative_path) });
    }

    return {};
}

static ErrorOr<void> collect_crash_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_crash_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }
        if (!is_valid_test_name(name))
            continue;

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Crash, input_path, {}, move(relative_path) });
    }

    return {};
}

static String generate_wait_for_test_string(StringView wait_class)
{
    return MUST(String::formatted(R"(
function hasTestWaitClass() {{
    return document.documentElement.classList.contains('{}');
}}

if (!hasTestWaitClass()) {{
    document.fonts.ready.then(() => {{
        requestAnimationFrame(function() {{
            requestAnimationFrame(function() {{
                internals.signalTestIsDone("PASS");
            }});
        }});
    }});
}} else {{
    const observer = new MutationObserver(() => {{
        if (!hasTestWaitClass()) {{
            internals.signalTestIsDone("PASS");
        }}
    }});

    observer.observe(document.documentElement, {{
        attributes: true,
        attributeFilter: ['class'],
    }});
}}
)"sv,
        wait_class));
}

static auto wait_for_crash_test_completion = generate_wait_for_test_string("test-wait"sv);
static auto wait_for_reftest_completion = generate_wait_for_test_string("reftest-wait"sv);

static ByteString test_mode_to_string(TestMode mode)
{
    switch (mode) {
    case TestMode::Layout:
        return "Layout"sv;
    case TestMode::Text:
        return "Text"sv;
    case TestMode::Ref:
        return "Ref"sv;
    case TestMode::Crash:
        return "Crash"sv;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<void> generate_result_files(Vector<TestCompletion> const& non_passing_tests)
{
    auto& app = Application::the();
    if (app.results_directory.is_empty())
        return {};

    // Count results
    size_t fail_count = 0;
    size_t timeout_count = 0;
    size_t crashed_count = 0;
    size_t skipped_count = 0;
    for (auto const& result : non_passing_tests) {
        switch (result.result) {
        case TestResult::Fail:
            ++fail_count;
            break;
        case TestResult::Timeout:
            ++timeout_count;
            break;
        case TestResult::Crashed:
            ++crashed_count;
            break;
        case TestResult::Skipped:
            ++skipped_count;
            break;
        default:
            break;
        }
    }

    // Write results.js (as JS to avoid fetch CORS issues with file://)
    StringBuilder js;
    js.append("const RESULTS_DATA = {\n"sv);
    js.appendff("  \"summary\": {{ \"total\": {}, \"fail\": {}, \"timeout\": {}, \"crashed\": {}, \"skipped\": {} }},\n",
        s_total_tests, fail_count, timeout_count, crashed_count, skipped_count);
    js.append("  \"tests\": [\n"sv);

    bool first = true;
    for (auto const& result : non_passing_tests) {
        if (result.result == TestResult::Skipped && app.verbosity < Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS)
            continue;

        if (!first)
            js.append(",\n"sv);
        first = false;

        auto base_path = LexicalPath::join(app.results_directory, result.test.relative_path).string();
        bool has_stdout = FileSystem::exists(ByteString::formatted("{}.stdout.txt", base_path));
        bool has_stderr = FileSystem::exists(ByteString::formatted("{}.stderr.txt", base_path));

        js.appendff("    {{ \"name\": \"{}\", \"result\": \"{}\", \"mode\": \"{}\", \"hasStdout\": {}, \"hasStderr\": {} }}",
            result.test.relative_path,
            test_result_to_string(result.result),
            test_mode_to_string(result.test.mode),
            has_stdout ? "true" : "false",
            has_stderr ? "true" : "false");
    }

    js.append("\n  ]\n};\n"sv);

    auto js_path = LexicalPath::join(app.results_directory, "results.js"sv).string();
    auto js_file = TRY(Core::File::open(js_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(js_file->write_until_depleted(js.string_view().bytes()));

    // Copy index.html from source tree
    auto source_html_path = LexicalPath::join(app.test_root_path, "test-web/results-index.html"sv).string();
    auto dest_html_path = LexicalPath::join(app.results_directory, "index.html"sv).string();
    auto source_html = TRY(Core::File::open(source_html_path, Core::File::OpenMode::Read));
    auto html_contents = TRY(source_html->read_until_eof());
    auto dest_html = TRY(Core::File::open(dest_html_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(dest_html->write_until_depleted(html_contents));

    return {};
}

static ErrorOr<void> write_test_diff_to_results(Test const& test, ByteBuffer const& expectation)
{
    auto& app = Application::the();
    if (app.results_directory.is_empty())
        return {};

    // Create the directory structure
    auto output_dir = LexicalPath::join(app.results_directory, LexicalPath::dirname(test.relative_path)).string();
    TRY(Core::Directory::create(output_dir, Core::Directory::CreateDirectories::Yes));

    auto base_path = LexicalPath::join(app.results_directory, test.relative_path).string();

    // Write expected output
    auto expected_path = ByteString::formatted("{}.expected.txt", base_path);
    auto expected_file = TRY(Core::File::open(expected_path, Core::File::OpenMode::Write));
    TRY(expected_file->write_until_depleted(expectation));

    // Write actual output
    auto actual_path = ByteString::formatted("{}.actual.txt", base_path);
    auto actual_file = TRY(Core::File::open(actual_path, Core::File::OpenMode::Write));
    TRY(actual_file->write_until_depleted(test.text.bytes()));

    // Write diff (plain text for tools)
    auto diff_path = ByteString::formatted("{}.diff.txt", base_path);
    auto diff_file = TRY(Core::File::open(diff_path, Core::File::OpenMode::Write));

    auto hunks = TRY(Diff::from_text(expectation, test.text, 3));
    TRY(Diff::write_unified_header(test.expectation_path, test.expectation_path, *diff_file));
    for (auto const& hunk : hunks)
        TRY(Diff::write_unified(hunk, *diff_file, Diff::ColorOutput::No));

    // Write diff (colorized HTML for viewer)
    auto html_path = ByteString::formatted("{}.diff.html", base_path);
    auto html_file = TRY(Core::File::open(html_path, Core::File::OpenMode::Write));

    TRY(html_file->write_until_depleted(R"html(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
body { margin: 0; background: #0d1117; }
pre { margin: 0; padding: 16px; font-family: ui-monospace, monospace; font-size: 12px; line-height: 1.5; }
.add { background: #12261e; color: #3fb950; border-left: 3px solid #238636; padding-left: 8px; margin-left: -11px; }
.del { background: #2d1619; color: #f85149; border-left: 3px solid #f85149; padding-left: 8px; margin-left: -11px; }
.hunk { color: #58a6ff; font-weight: 500; }
.ctx { color: #8b949e; }
</style>
</head>
<body><pre>)html"sv));

    // Write header
    TRY(html_file->write_until_depleted("<span class=\"ctx\">"sv));
    TRY(html_file->write_formatted("--- {}\n", test.expectation_path));
    TRY(html_file->write_formatted("+++ {}\n", test.expectation_path));
    TRY(html_file->write_until_depleted("</span>"sv));

    // Write hunks with colorization
    for (auto const& hunk : hunks) {
        TRY(html_file->write_formatted("<span class=\"hunk\">{}</span>\n", hunk.location));

        for (auto const& line : hunk.lines) {
            auto escaped = escape_html_entities(line.content);
            switch (line.operation) {
            case Diff::Line::Operation::Addition:
                TRY(html_file->write_formatted("<span class=\"add\">+{}</span>\n", escaped));
                break;
            case Diff::Line::Operation::Removal:
                TRY(html_file->write_formatted("<span class=\"del\">-{}</span>\n", escaped));
                break;
            case Diff::Line::Operation::Context:
                TRY(html_file->write_formatted("<span class=\"ctx\"> {}</span>\n", escaped));
                break;
            }
        }
    }

    TRY(html_file->write_until_depleted("</pre></body></html>"sv));

    return {};
}

static void run_dump_test(TestWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    test.timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&test, url]() -> ErrorOr<TestResult> {
        if (test.expectation_path.is_empty()) {
            if (test.mode != TestMode::Crash)
                outln("{}", test.text);
            return TestResult::Pass;
        }

        auto open_expectation_file = [&](auto mode) {
            auto expectation_file_or_error = Core::File::open(test.expectation_path, mode);
            if (expectation_file_or_error.is_error())
                warnln("Failed opening '{}': {}", test.expectation_path, expectation_file_or_error.error());

            return expectation_file_or_error;
        };

        ByteBuffer expectation;

        if (auto expectation_file = open_expectation_file(Core::File::OpenMode::Read); !expectation_file.is_error()) {
            expectation = TRY(expectation_file.value()->read_until_eof());

            auto result_trimmed = StringView { test.text }.trim("\n"sv, TrimMode::Right);
            auto expectation_trimmed = StringView { expectation }.trim("\n"sv, TrimMode::Right);

            if (result_trimmed == expectation_trimmed)
                return TestResult::Pass;
        } else if (!Application::the().rebaseline) {
            return expectation_file.release_error();
        }

        if (Application::the().rebaseline) {
            TRY(Core::Directory::create(LexicalPath { test.expectation_path }.parent().string(), Core::Directory::CreateDirectories::Yes));

            auto expectation_file = TRY(open_expectation_file(Core::File::OpenMode::Write));
            TRY(expectation_file->write_until_depleted(test.text));

            return TestResult::Pass;
        }

        // Write diff to results directory if specified
        TRY(write_test_diff_to_results(test, expectation));

        // Only output to stdout if not using results directory (for CI compatibility)
        if (Application::the().results_directory.is_empty()) {
            auto const color_output = TRY(Core::System::isatty(STDOUT_FILENO)) ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;

            if (color_output == Diff::ColorOutput::Yes)
                outln("\n\033[33;1mTest failed\033[0m: {}", url);
            else
                outln("\nTest failed: {}", url);

            auto hunks = TRY(Diff::from_text(expectation, test.text, 3));
            auto out = TRY(Core::File::standard_output());

            TRY(Diff::write_unified_header(test.expectation_path, test.expectation_path, *out));
            for (auto const& hunk : hunks)
                TRY(Diff::write_unified(hunk, *out, color_output));
        }

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, handle_completed_test]() {
        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    if (test.mode == TestMode::Layout) {
        view.on_load_finish = [&view, &test, url, on_test_complete = move(on_test_complete)](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            // NOTE: We take a screenshot here to force the lazy layout of SVG-as-image documents to happen.
            //       It also causes a lot more code to run, which is good for finding bugs. :^)
            view.take_screenshot()->when_resolved([&view, &test, on_test_complete = move(on_test_complete)](auto) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree | WebView::PageInfoType::StackingContextTree);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            });
        };
    } else if (test.mode == TestMode::Text) {
        view.on_load_finish = [&view, &test, on_test_complete, url](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            test.did_finish_loading = true;

            if (test.expectation_path.is_empty()) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::Text);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            } else if (test.did_finish_test) {
                on_test_complete();
            }
        };

        view.on_test_finish = [&test, on_test_complete](auto const& text) {
            test.text = text;
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    } else if (test.mode == TestMode::Crash) {
        view.on_load_finish = [on_test_complete, url, &view, &test](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            test.did_finish_loading = true;
            view.run_javascript(wait_for_crash_test_completion);

            if (test.did_finish_test)
                on_test_complete();
        };

        view.on_test_finish = [&test, on_test_complete](auto const&) {
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    }

    view.on_set_test_timeout = [&test, timeout_in_milliseconds](double milliseconds) {
        if (milliseconds > timeout_in_milliseconds)
            test.timeout_timer->restart(milliseconds);
    };

    view.load(url);
    test.timeout_timer->start();
}

static void run_ref_test(TestWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    test.timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&view, &test, url]() -> ErrorOr<TestResult> {
        VERIFY(test.ref_test_expectation_type.has_value());
        auto should_match = test.ref_test_expectation_type == RefTestExpectationType::Match;
        auto screenshot_matches = fuzzy_screenshot_match(
            url, view.url(), *test.actual_screenshot, *test.expectation_screenshot, test.fuzzy_matches);
        if (should_match == screenshot_matches)
            return TestResult::Pass;

        auto& app = Application::the();

        auto dump_screenshot = [](Gfx::Bitmap const& bitmap, StringView path) -> ErrorOr<void> {
            auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
            auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
            TRY(screenshot_file->write_until_depleted(encoded_data));
            return {};
        };

        // Save to results directory if specified
        if (!app.results_directory.is_empty()) {
            auto output_dir = LexicalPath::join(app.results_directory, LexicalPath::dirname(test.relative_path)).string();
            TRY(Core::Directory::create(output_dir, Core::Directory::CreateDirectories::Yes));

            auto base_path = LexicalPath::join(app.results_directory, test.relative_path).string();
            TRY(dump_screenshot(*test.actual_screenshot, ByteString::formatted("{}.actual.png", base_path)));
            TRY(dump_screenshot(*test.expectation_screenshot, ByteString::formatted("{}.expected.png", base_path)));
        } else if (app.dump_failed_ref_tests) {
            warnln("\033[33;1mRef test {} failed; dumping screenshots\033[0m", test.relative_path);

            TRY(Core::Directory::create("test-dumps"sv, Core::Directory::CreateDirectories::Yes));

            auto title = LexicalPath::title(URL::percent_decode(url.serialize_path()));
            TRY(dump_screenshot(*test.actual_screenshot, ByteString::formatted("test-dumps/{}.png", title)));
            TRY(dump_screenshot(*test.expectation_screenshot, ByteString::formatted("test-dumps/{}-ref.png", title)));

            outln("\033[33;1mDumped test-dumps/{}.png\033[0m", title);
            outln("\033[33;1mDumped test-dumps/{}-ref.png\033[0m", title);
        }

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, handle_completed_test]() {
        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_load_finish = [&view](auto const&) {
        view.run_javascript(wait_for_reftest_completion);
    };

    view.on_test_finish = [&view, &test, on_test_complete = move(on_test_complete)](auto const&) {
        if (test.actual_screenshot) {
            // The reference has finished loading; take another screenshot and move on to handling the result.
            view.take_screenshot()->when_resolved([&view, &test, on_test_complete = move(on_test_complete)](RefPtr<Gfx::Bitmap const> screenshot) {
                test.expectation_screenshot = move(screenshot);
                view.reset_zoom();
                on_test_complete();
            });
        } else {
            // When the test initially finishes, we take a screenshot and request the reference test metadata.
            view.take_screenshot()->when_resolved([&view, &test](RefPtr<Gfx::Bitmap const> screenshot) {
                test.actual_screenshot = move(screenshot);
                view.reset_zoom();
                view.run_javascript("internals.loadReferenceTestMetadata();"_string);
            });
        }
    };

    view.on_reference_test_metadata = [&view, &test](JsonValue const& metadata) {
        auto metadata_object = metadata.as_object();

        auto match_references = metadata_object.get_array("match_references"sv);
        auto mismatch_references = metadata_object.get_array("mismatch_references"sv);
        if (match_references->is_empty() && mismatch_references->is_empty()) {
            dbgln("No match or mismatch references in `{}`! Metadata: {}", view.url(), metadata_object.serialized());
            VERIFY_NOT_REACHED();
        }

        // Read fuzzy configurations.
        test.fuzzy_matches.clear_with_capacity();
        auto fuzzy_values = metadata_object.get_array("fuzzy"sv);
        for (size_t i = 0; i < fuzzy_values->size(); ++i) {
            auto fuzzy_configuration = fuzzy_values->at(i).as_object();

            Optional<URL::URL> reference_url;
            auto reference = fuzzy_configuration.get_string("reference"sv);
            if (reference.has_value())
                reference_url = URL::Parser::basic_parse(reference.release_value());

            auto content = fuzzy_configuration.get_string("content"sv).release_value();
            auto fuzzy_match_or_error = parse_fuzzy_match(reference_url, content);
            if (fuzzy_match_or_error.is_error()) {
                warnln("Failed to parse fuzzy configuration '{}' (reference: {}): {}", content, reference_url, fuzzy_match_or_error.error());
                continue;
            }

            test.fuzzy_matches.append(fuzzy_match_or_error.release_value());
        }

        // Read (mis)match reference tests to load.
        // FIXME: Currently we only support single match or mismatch reference.
        String reference_to_load;
        if (!match_references->is_empty()) {
            if (match_references->size() > 1)
                dbgln("FIXME: Only a single ref test match reference is supported");

            test.ref_test_expectation_type = RefTestExpectationType::Match;
            reference_to_load = match_references->at(0).as_string();
        } else {
            if (mismatch_references->size() > 1)
                dbgln("FIXME: Only a single ref test mismatch reference is supported");

            test.ref_test_expectation_type = RefTestExpectationType::Mismatch;
            reference_to_load = mismatch_references->at(0).as_string();
        }
        view.load(URL::Parser::basic_parse(reference_to_load).release_value());
    };

    view.on_set_test_timeout = [&test, timeout_in_milliseconds](double milliseconds) {
        if (milliseconds > timeout_in_milliseconds)
            test.timeout_timer->restart(milliseconds);
    };

    view.load(url);
    test.timeout_timer->start();
}

static void run_test(TestWebView& view, Test& test, Application& app)
{
    s_test_by_view.set(&view, &test);

    // Clear the current document.
    // FIXME: Implement a debug-request to do this more thoroughly.
    auto promise = Core::Promise<Empty>::construct();

    view.on_load_finish = [promise](auto const& url) {
        if (!url.equals(URL::about_blank()))
            return;

        Core::deferred_invoke([promise]() {
            promise->resolve({});
        });
    };

    view.on_test_finish = {};

    promise->when_resolved([&view, &test, &app](auto) {
        auto url = URL::create_with_file_scheme(MUST(FileSystem::real_path(test.input_path))).release_value();

        switch (test.mode) {
        case TestMode::Crash:
        case TestMode::Text:
        case TestMode::Layout:
            run_dump_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        case TestMode::Ref:
            run_ref_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        }

        VERIFY_NOT_REACHED();
    });

    view.load(URL::about_blank());
}

static void set_ui_callbacks_for_tests(TestWebView& view)
{
    view.on_request_file_picker = [&](auto const& accepted_file_types, auto allow_multiple_files) {
        // Create some dummy files for tests.
        Vector<Web::HTML::SelectedFile> selected_files;

        bool add_txt_files = accepted_file_types.filters.is_empty();
        bool add_cpp_files = false;

        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [](Web::HTML::FileFilter::FileType) {},
                [&](Web::HTML::FileFilter::MimeType const& mime_type) {
                    if (mime_type.value == "text/plain"sv)
                        add_txt_files = true;
                },
                [&](Web::HTML::FileFilter::Extension const& extension) {
                    if (extension.value == "cpp"sv)
                        add_cpp_files = true;
                });
        }

        if (add_txt_files) {
            selected_files.empend("file1"sv, MUST(ByteBuffer::copy("Contents for file1"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2"sv, MUST(ByteBuffer::copy("Contents for file2"sv.bytes())));
                selected_files.empend("file3"sv, MUST(ByteBuffer::copy("Contents for file3"sv.bytes())));
                selected_files.empend("file4"sv, MUST(ByteBuffer::copy("Contents for file4"sv.bytes())));
            }
        }

        if (add_cpp_files) {
            selected_files.empend("file1.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 1; }}"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 2; }}"sv.bytes())));
            }
        }

        view.file_picker_closed(move(selected_files));
    };

    view.on_request_alert = [&](auto const&) {
        // For tests, just close the alert right away to unblock JS execution.
        view.alert_closed();
    };

    view.on_web_content_crashed = [&view]() {
        if (auto test = s_test_by_view.get(&view); test.has_value())
            view.on_test_complete({ *test.value(), TestResult::Crashed });
    };
}

static ErrorOr<int> run_tests(Core::AnonymousBuffer const& theme, Web::DevicePixelSize window_size)
{
    auto& app = Application::the();
    TRY(load_test_config(app.test_root_path));

    Vector<Test> tests;

    for (auto& glob : app.test_globs)
        glob = ByteString::formatted("*{}*", glob);
    if (app.test_globs.is_empty())
        app.test_globs.append("*"sv);

    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Layout", app.test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Text", app.test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(app, tests, ByteString::formatted("{}/Ref", app.test_root_path), "."sv));
    TRY(collect_crash_tests(app, tests, ByteString::formatted("{}/Crash", app.test_root_path), "."sv));
    TRY(collect_ref_tests(app, tests, ByteString::formatted("{}/Screenshot", app.test_root_path), "."sv));

    tests.remove_all_matching([&](auto const& test) {
        static constexpr Array support_file_patterns {
            "*/wpt-import/*/support/*"sv,
            "*/wpt-import/*/resources/*"sv,
            "*/wpt-import/common/*"sv,
            "*/wpt-import/images/*"sv,
        };
        bool is_support_file = any_of(support_file_patterns, [&](auto pattern) { return test.input_path.matches(pattern); });
        bool match_glob = any_of(app.test_globs, [&](auto const& glob) { return test.relative_path.matches(glob, CaseSensitivity::CaseSensitive); });
        return is_support_file || !match_glob;
    });

    if (app.shuffle)
        shuffle(tests);

    if (app.test_dry_run) {
        outln("Found {} tests...", tests.size());

        for (auto const& [i, test] : enumerate(tests))
            outln("{}/{}: {}", i + 1, tests.size(), test.relative_path);

        return 0;
    }

    if (tests.is_empty()) {
        if (app.test_globs.is_empty())
            return Error::from_string_literal("No tests found");
        return Error::from_string_literal("No tests found matching filter");
    }

    auto concurrency = min(app.test_concurrency, tests.size());
    size_t loaded_web_views = 0;

    Vector<NonnullOwnPtr<TestWebView>> views;
    views.ensure_capacity(concurrency);

    for (size_t i = 0; i < concurrency; ++i) {
        auto view = TestWebView::create(theme, window_size);
        view->on_load_finish = [&](auto const&) { ++loaded_web_views; };
        // FIXME: Figure out a better way to ensure that tests use default browser settings.
        view->reset_zoom();

        views.unchecked_append(move(view));
    }

    // We need to wait for the initial about:blank load to complete before starting the tests, otherwise we may load the
    // test URL before the about:blank load completes. WebContent currently cannot handle this, and will drop the test URL.
    Core::EventLoop::current().spin_until([&]() {
        return loaded_web_views == concurrency;
    });

    // Set up output capture for each view if results directory is specified
    for (auto& view : views)
        setup_output_capture_for_view(*view);

    // Initialize live terminal display
    s_is_tty = TRY(Core::System::isatty(STDOUT_FILENO));
    if (s_is_tty) {
        update_terminal_size();

#ifndef AK_OS_WINDOWS
        // Handle terminal resize
        Core::EventLoop::register_signal(SIGWINCH, [](int) {
            Core::EventLoop::current().deferred_invoke([] {
                update_terminal_size();
            });
        });
#endif

        // Initialize view display states
        s_view_display_states.resize(concurrency);
        for (auto [i, view] : enumerate(views)) {
            s_view_display_states[i].pid = view->web_content_pid();
            s_view_display_states[i].active = false;
        }

        // Start 1-second timer for display updates
        s_display_timer = Core::Timer::create_repeating(1000, [] {
            render_live_display();
        });
        s_display_timer->start();
    }

    // Reset counters for this run
    s_pass_count = 0;
    s_fail_count = 0;
    s_timeout_count = 0;
    s_crashed_count = 0;
    s_skipped_count = 0;
    s_completed_tests = 0;

    // When on TTY with live display, use the N-line display; otherwise use single-line or verbose
    bool use_live_display = s_is_tty && app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_DURATION;
    s_total_tests = tests.size();
    outln("Running {} tests...", tests.size());

    // Set up display area for live display
    if (use_live_display) {
        s_live_display_lines = concurrency + 4; // +1 empty, +1 status counts, +1 empty, +1 progress bar
        for (size_t i = 0; i < s_live_display_lines; ++i)
            outln();
        (void)fflush(stdout);
    }

    s_all_tests_complete = Core::Promise<Empty>::construct();
    auto tests_remaining = tests.size();
    auto current_test = 0uz;

    Vector<TestCompletion> non_passing_tests;

    auto digits_for_view_id = static_cast<size_t>(log10(views.size()) + 1);
    auto digits_for_test_id = static_cast<size_t>(log10(tests.size()) + 1);

    for (auto [view_id, view] : enumerate(views)) {
        set_ui_callbacks_for_tests(*view);
        view->clear_content_filters();

        auto run_next_test = [&, view_id]() {
            auto index = current_test++;
            if (index >= tests.size()) {
                // Mark this view as idle
                if (use_live_display && view_id < s_view_display_states.size())
                    s_view_display_states[view_id].active = false;
                return;
            }

            auto& test = tests[index];
            test.start_time = UnixDateTime::now();
            test.index = index + 1;

            if (use_live_display) {
                // Update view display state for live display (refresh PID in case WebContent respawned)
                if (view_id < s_view_display_states.size()) {
                    s_view_display_states[view_id].pid = view->web_content_pid();
                    s_view_display_states[view_id].test_name = test.relative_path;
                    s_view_display_states[view_id].start_time = test.start_time;
                    s_view_display_states[view_id].active = true;
                }
                render_live_display();
            } else {
                // Non-TTY mode: print each test as it starts
                outln("{}/{}: {}", test.index, tests.size(), test.relative_path);
            }

            Core::deferred_invoke([&]() mutable {
                if (s_skipped_tests.contains_slow(test.input_path))
                    view->on_test_complete({ test, TestResult::Skipped });
                else
                    run_test(*view, test, app);
            });
        };

        view->test_promise().when_resolved([&, run_next_test, view_id](auto result) {
            view->on_load_finish = {};
            view->on_test_finish = {};
            view->on_reference_test_metadata = {};
            view->on_set_test_timeout = {};

            // Don't try to reset zoom if WebContent crashed - it's gone
            if (result.result != TestResult::Crashed)
                view->reset_zoom();

            if (result.test.timeout_timer) {
                result.test.timeout_timer->stop();
                result.test.timeout_timer.clear();
            }

            result.test.end_time = UnixDateTime::now();
            s_test_by_view.remove(view);

            // Write captured stdout/stderr to results directory
            if (auto capture = s_output_captures.get(view); capture.has_value() && *capture)
                (void)write_output_for_test(result.test, **capture);

            if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_DURATION) {
                auto duration = result.test.end_time - result.test.start_time;
                outln("[{:{}}] {:{}}/{}: Finish {}: {}ms", view_id, digits_for_view_id, result.test.index, digits_for_test_id, tests.size(), result.test.relative_path, duration.to_milliseconds());
            }

            switch (result.result) {
            case TestResult::Pass:
                ++s_pass_count;
                break;
            case TestResult::Fail:
                ++s_fail_count;
                break;
            case TestResult::Timeout:
                ++s_timeout_count;
                break;
            case TestResult::Crashed:
                ++s_crashed_count;
                break;
            case TestResult::Skipped:
                ++s_skipped_count;
                break;
            }

            ++s_completed_tests;

            if (result.result != TestResult::Pass)
                non_passing_tests.append(move(result));

            if (--tests_remaining == 0)
                s_all_tests_complete->resolve({});
            else
                run_next_test();
        });

        Core::deferred_invoke([run_next_test]() {
            run_next_test();
        });
    }

    auto result_or_rejection = s_all_tests_complete->await();

    // Stop the live display timer
    if (s_display_timer) {
        s_display_timer->stop();
        s_display_timer = nullptr;
    }

    // Clear the live display area and move cursor back up
    if (use_live_display) {
        for (size_t i = 0; i < s_live_display_lines; ++i)
            out("\033[A\033[2K"sv); // Move up and clear each line
        out("\r"sv);
        (void)fflush(stdout);

        // Print any warnings that were deferred during live display
        s_live_display_lines = 0;
        print_deferred_warnings();
    }

    if (result_or_rejection.is_error())
        outln("Halted; {} tests not executed.", tests_remaining);

    outln("==========================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}, Crashed: {}", s_pass_count, s_fail_count, s_skipped_count, s_timeout_count, s_crashed_count);
    outln("==========================================================");

    for (auto const& non_passing_test : non_passing_tests) {
        if (non_passing_test.result == TestResult::Skipped && app.verbosity < Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS)
            continue;

        outln("{}: {}", test_result_to_string(non_passing_test.result), non_passing_test.test.relative_path);
    }

    if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_SLOWEST_TESTS) {
        auto tests_to_print = min(10uz, tests.size());
        outln("\nSlowest {} tests:", tests_to_print);

        quick_sort(tests, [&](auto const& lhs, auto const& rhs) {
            auto lhs_duration = lhs.end_time - lhs.start_time;
            auto rhs_duration = rhs.end_time - rhs.start_time;
            return lhs_duration > rhs_duration;
        });

        for (auto const& test : tests.span().trim(tests_to_print)) {
            auto duration = test.end_time - test.start_time;

            outln("{}: {}ms", test.relative_path, duration.to_milliseconds());
        }
    }

    if (app.dump_gc_graph) {
        for (auto& view : views) {
            if (auto path = view->dump_gc_graph(); path.is_error())
                warnln("Failed to dump GC graph: {}", path.error());
            else
                outln("GC graph dumped to {}", path.value());
        }
    }

    // Generate result files (JSON data and HTML index)
    if (auto result = generate_result_files(non_passing_tests); result.is_error())
        warnln("Failed to generate result files: {}", result.error());
    else if (!app.results_directory.is_empty())
        outln("Results: file://{}/index.html", app.results_directory);

    return s_fail_count + s_timeout_count + s_crashed_count + tests_remaining;
}

static void handle_signal(int signal)
{
    VERIFY(signal == SIGINT || signal == SIGTERM);

    // Quit our event loop. This makes `::exec()` return as soon as possible, and signals to WebView::Application that
    // we should no longer automatically restart processes in `::process_did_exit()`.
    Core::EventLoop::current().quit(0);

    // Report current view statuses
    dbgln();
    dbgln("{} received. Active test views:", signal == SIGINT ? "SIGINT"sv : "SIGTERM"sv);
    dbgln();

    auto now = UnixDateTime::now();
    WebView::ViewImplementation::for_each_view([&](WebView::ViewImplementation const& view) {
        dbg("- View {}: ", view.view_id());

        auto maybe_test = s_test_by_view.get(&view);
        if (maybe_test.has_value()) {
            auto const& test = *maybe_test.release_value();
            dbgln("{} (duration: {})", test.relative_path, human_readable_time(now - test.start_time));
        } else {
            dbgln("{} (no active test)", view.url());
        }

        return IterationDecision::Continue;
    });
    dbgln();

    // Stop running tests
    s_all_tests_complete->reject(signal == SIGINT
            ? Error::from_string_view("SIGINT received"sv)
            : Error::from_string_view("SIGTERM received"sv));
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestWeb::Application::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestWeb::Application::create(arguments, OptionalNone {}));
#endif

    Core::EventLoop::register_signal(SIGINT, TestWeb::handle_signal);
    Core::EventLoop::register_signal(SIGTERM, TestWeb::handle_signal);

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto const& browser_options = TestWeb::Application::browser_options();
    Web::DevicePixelSize window_size { browser_options.window_width, browser_options.window_height };

    VERIFY(!app->test_root_path.is_empty());

    app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);

    // Set default results directory if not specified
    if (app->results_directory.is_empty())
        app->results_directory = "test-dumps/results"sv;

    app->results_directory = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->results_directory);
    TRY(Core::Directory::create(app->results_directory, Core::Directory::CreateDirectories::Yes));

    TRY(app->launch_test_fixtures());

    return TestWeb::run_tests(theme, window_size);
}
