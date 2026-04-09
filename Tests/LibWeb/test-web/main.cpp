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
#include "Debug.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/ByteBuffer.h>
#include <AK/Enumerate.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/NumberFormat.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Span.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibCore/Notifier.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/ImageDecoder.h>
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

static size_t s_current_run = 1;

static bool s_fail_fast_triggered = false;

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
static Vector<Function<void()>> s_view_run_next_test;
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
                auto duration = (now - state.start_time).to_truncated_seconds();
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
        auto counter_start = output.length();
        output.appendff("{}/{} ", completed, total);
        if (Application::the().repeat_count > 1)
            output.appendff("run {}/{} ", s_current_run, Application::the().repeat_count);
        auto counter_length = output.length() - counter_start;
        size_t bar_width = s_terminal_width > counter_length + 3 ? s_terminal_width - counter_length - 3 : 20;

        size_t filled = total > 0 ? (completed * bar_width) / total : 0;
        size_t empty = bar_width - filled;

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
static Vector<ByteString> s_loaded_from_http_server;
static HashMap<WebView::ViewImplementation const*, size_t> s_current_test_index_by_view;

struct TestRunContext {
    Vector<Test>& tests;
    size_t& tests_remaining;
    size_t& total_tests;
};

static TestRunContext* s_run_context { nullptr };

struct ViewOutputCapture {
    StringBuilder stdout_buffer;
    StringBuilder stderr_buffer;
    RefPtr<Core::Notifier> stdout_notifier;
    RefPtr<Core::Notifier> stderr_notifier;
};

static HashMap<WebView::ViewImplementation const*, NonnullOwnPtr<ViewOutputCapture>> s_output_captures;

static ViewOutputCapture& output_capture_for_view(WebView::ViewImplementation const& view)
{
    auto capture = s_output_captures.get(&view);
    VERIFY(capture.has_value());
    return *capture.value();
}

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
            if (nread > 0) {
                StringView message { buffer, static_cast<size_t>(nread) };

                if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                    (void)Core::System::write(STDOUT_FILENO, message.bytes());

                capture.stdout_buffer.append(message);
            } else {
                capture.stdout_notifier->set_enabled(false);
            }
        };
    }

    if (output_capture.stderr_file) {
        auto fd = output_capture.stderr_file->fd();
        view_capture->stderr_notifier = Core::Notifier::construct(fd, Core::Notifier::Type::Read);
        view_capture->stderr_notifier->on_activation = [fd, &capture = *view_capture]() {
            char buffer[4096];
            auto nread = read(fd, buffer, sizeof(buffer));
            if (nread > 0) {
                StringView message { buffer, static_cast<size_t>(nread) };

                if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                    (void)Core::System::write(STDERR_FILENO, message.bytes());

                capture.stderr_buffer.append(message);
            } else {
                capture.stderr_notifier->set_enabled(false);
            }
        };
    }

    s_output_captures.set(&view, move(view_capture));
}

static ErrorOr<ByteString> prepare_output_path(Test const& test)
{
    auto& app = Application::the();
    auto base_path = LexicalPath::join(app.results_directory, test.safe_relative_path);
    TRY(Core::Directory::create(base_path.dirname(), Core::Directory::CreateDirectories::Yes));
    return base_path.string();
}

static ErrorOr<void> write_output_for_test(Test const& test, ViewOutputCapture& capture)
{
    auto base_path = TRY(prepare_output_path(test));

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
        auto stderr_view = capture.stderr_buffer.string_view();
        if (stderr_view.contains('\x1b')) {
            auto stripped = strip_sgr_sequences(stderr_view);
            TRY(file->write_until_depleted(stripped.bytes()));
        } else {
            TRY(file->write_until_depleted(stderr_view.bytes()));
        }
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
    case TestResult::Expanded:
        return "Expanded"sv;
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
        } else if (group == "LoadFromHttpServer"sv) {
            for (auto& key : config->keys(group))
                s_loaded_from_http_server.append(TRY(FileSystem::real_path(LexicalPath::join(test_root_path, key).string())));
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
        tests.append({ mode, input_path, move(expectation_path), relative_path, relative_path });
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

        if (!is_valid_test_name(name))
            continue;

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Ref, input_path, {}, relative_path, relative_path });
    }

    return {};
}

static ErrorOr<void> collect_screenshot_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_screenshot_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.png", path, trail, LexicalPath::title(name));
        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Screenshot, input_path, move(expectation_path), relative_path, relative_path });
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
        tests.append({ TestMode::Crash, input_path, {}, relative_path, relative_path });
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
    case TestMode::Screenshot:
        return "Screenshot"sv;
    case TestMode::Crash:
        return "Crash"sv;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<void> generate_result_files(ReadonlySpan<Test> tests, ReadonlySpan<TestCompletion> non_passing_tests)
{
    auto& app = Application::the();

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

        auto const& test = tests[result.test_index];
        auto base_path = TRY(prepare_output_path(test));
        bool has_stdout = FileSystem::exists(ByteString::formatted("{}.stdout.txt", base_path));
        bool has_stderr = FileSystem::exists(ByteString::formatted("{}.stderr.txt", base_path));

        js.appendff("    {{ \"name\": \"{}\", \"result\": \"{}\", \"mode\": \"{}\", \"hasStdout\": {}, \"hasStderr\": {}",
            test.safe_relative_path,
            test_result_to_string(result.result),
            test_mode_to_string(test.mode),
            has_stdout ? "true" : "false",
            has_stderr ? "true" : "false");
        if ((test.mode == TestMode::Ref || test.mode == TestMode::Screenshot) && test.diff_pixel_error_count > 0)
            js.appendff(", \"pixelErrors\": {}, \"maxChannelDiff\": {}", test.diff_pixel_error_count, test.diff_maximum_error);
        js.append(" }"sv);
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
    auto base_path = TRY(prepare_output_path(test));

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

static void expand_test_with_variants(TestRunContext& context, size_t base_test_index, ReadonlySpan<String> variants)
{
    VERIFY(!variants.is_empty());

    context.tests.ensure_capacity(context.tests.size() + variants.size());
    auto const& base_test = context.tests[base_test_index];

    for (auto const& variant : variants) {
        Test variant_test;
        variant_test.mode = base_test.mode;
        variant_test.run_index = base_test.run_index;
        variant_test.total_runs = base_test.total_runs;
        variant_test.input_path = base_test.input_path;
        variant_test.variant = variant;

        // relative_path uses '?' for display, safe_relative_path uses '@' for filesystem
        auto variant_suffix = StringView { variant }.substring_view(1);
        variant_test.relative_path = ByteString::formatted("{}?{}", base_test.relative_path, variant_suffix);
        variant_test.safe_relative_path = ByteString::formatted("{}@{}", base_test.safe_relative_path, variant_suffix);

        // Expected file: test@variant_suffix.txt
        auto dir = LexicalPath::dirname(base_test.expectation_path);
        auto title = LexicalPath::title(LexicalPath::basename(base_test.input_path));
        if (dir.is_empty())
            variant_test.expectation_path = ByteString::formatted("{}@{}.txt", title, variant_suffix);
        else
            variant_test.expectation_path = ByteString::formatted("{}/{}@{}.txt", dir, title, variant_suffix);

        // Set the index before appending so it matches the position in the vector
        variant_test.index = context.tests.size();
        context.tests.unchecked_append(move(variant_test));
    }

    // Add variants.size() because the original test will decrement tests_remaining when
    // it completes as Expanded, and each variant will also decrement when it completes.
    context.tests_remaining += variants.size();

    // For display, add (variants.size() - 1) since Expanded tests don't count in s_completed_tests
    context.total_tests += variants.size() - 1;
}

static void run_dump_test(TestWebView& view, TestRunContext& context, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    auto test_index = test.index;
    test.timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, test_index]() {
        view.on_test_complete({ test_index, TestResult::Timeout });
    });

    auto handle_completed_test = [&context, test_index, url]() -> ErrorOr<TestResult> {
        auto& test = context.tests[test_index];
        if (test.expectation_path.is_empty()) {
            if (test.mode != TestMode::Crash)
                outln("{}", test.text);
            return TestResult::Pass;
        }

        auto open_expectation_file = [&](auto mode) {
            auto expectation_file_or_error = Core::File::open(test.expectation_path, mode);
            if (expectation_file_or_error.is_error())
                add_deferred_warning(ByteString::formatted("Failed opening '{}': {}", test.expectation_path, expectation_file_or_error.error()));

            return expectation_file_or_error;
        };

        ByteBuffer expectation;

        if (auto expectation_file = open_expectation_file(Core::File::OpenMode::Read); !expectation_file.is_error()) {
            expectation = TRY(expectation_file.value()->read_until_eof());

            auto result_trimmed = StringView { test.text }.trim("\n"sv, TrimMode::Right);
            auto expectation_trimmed = StringView { expectation }.trim("\n"sv, TrimMode::Right);

            if (result_trimmed == expectation_trimmed)
                return TestResult::Pass;
        }

        if (Application::the().rebaseline) {
            TRY(Core::Directory::create(LexicalPath { test.expectation_path }.parent().string(), Core::Directory::CreateDirectories::Yes));

            auto expectation_file = TRY(open_expectation_file(Core::File::OpenMode::Write));
            TRY(expectation_file->write_until_depleted(test.text));

            return TestResult::Pass;
        }

        TRY(write_test_diff_to_results(test, expectation));

        if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT) {
            auto const color_output = s_is_tty ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;

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

    auto on_test_complete = [&view, test_index, handle_completed_test]() {
        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test_index, TestResult::Fail });
        else
            view.on_test_complete({ test_index, result.value() });
    };

    if (test.mode == TestMode::Layout) {
        view.on_load_finish = [&view, &context, test_index, url, on_test_complete = move(on_test_complete)](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            // NOTE: We take a screenshot here to force the lazy layout of SVG-as-image documents to happen.
            //       It also causes a lot more code to run, which is good for finding bugs. :^)
            view.take_screenshot()->when_resolved([&view, &context, test_index, on_test_complete = move(on_test_complete)](auto const&) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree | WebView::PageInfoType::StackingContextTree);

                promise->when_resolved([&context, test_index, on_test_complete = move(on_test_complete)](auto const& text) {
                    context.tests[test_index].text = text;
                    on_test_complete();
                });
            });
        };
    } else if (test.mode == TestMode::Text) {
        // Set up variant detection callback.
        view.on_test_variant_metadata = [&view, &context, test_index, on_test_complete](JsonValue metadata) {
            // Verify this IPC response is for the current test on this view (use index to avoid dangling pointer issues)
            auto current_index = s_current_test_index_by_view.get(&view);
            if (!current_index.has_value() || *current_index != test_index)
                return;

            auto& test = context.tests[test_index];
            if (test.variant.has_value())
                return;

            auto const& variants_array = metadata.as_array();

            if (!variants_array.is_empty()) {
                Vector<String> variants;
                variants.ensure_capacity(variants_array.size());
                for (auto const& variant : variants_array.values())
                    variants.unchecked_append(variant.as_string());

                expand_test_with_variants(context, test_index, variants);
                view.on_test_complete({ test_index, TestResult::Expanded });
                return;
            }

            auto& test_after_check = context.tests[test_index];
            test_after_check.did_check_variants = true;
            if (test_after_check.did_finish_test)
                on_test_complete();
        };

        view.on_load_finish = [&view, &context, test_index, on_test_complete, url](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            auto& test = context.tests[test_index];
            test.did_finish_loading = true;

            if (!test.variant.has_value())
                view.run_javascript("internals.loadTestVariants();"_string);
            else
                test.did_check_variants = true;

            if (test.expectation_path.is_empty()) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::Text);

                promise->when_resolved([&context, test_index, on_test_complete = move(on_test_complete)](auto const& text) {
                    auto& test = context.tests[test_index];
                    test.text = text;
                    on_test_complete();
                });
            } else if (test.did_finish_test && test.did_check_variants) {
                on_test_complete();
            }
        };

        view.on_test_finish = [&context, test_index, on_test_complete](auto const& text) {
            auto& test = context.tests[test_index];
            test.text = text;
            test.did_finish_test = true;

            if (test.did_finish_loading && test.did_check_variants)
                on_test_complete();
        };
    } else if (test.mode == TestMode::Crash) {
        view.on_load_finish = [on_test_complete, url, &view, &context, test_index](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            auto& test = context.tests[test_index];
            test.did_finish_loading = true;
            view.run_javascript(wait_for_crash_test_completion);

            if (test.did_finish_test)
                on_test_complete();
        };

        view.on_test_finish = [&context, test_index, on_test_complete](auto const&) {
            auto& test = context.tests[test_index];
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    }

    view.on_set_test_timeout = [&context, test_index, timeout_in_milliseconds](double milliseconds) {
        auto& test = context.tests[test_index];
        if (milliseconds > timeout_in_milliseconds)
            test.timeout_timer->restart(AK::clamp_to<int>(milliseconds));
    };

    view.load(url);
    test.timeout_timer->start();
}

static ErrorOr<void> dump_screenshot_to_file(Gfx::Bitmap const& bitmap, StringView path)
{
    auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
    TRY(screenshot_file->write_until_depleted(encoded_data));
    return {};
}

static ErrorOr<void> write_screenshot_failure_results(Test& test, Gfx::Bitmap const& actual, Gfx::Bitmap const& expected)
{
    auto base_path = TRY(prepare_output_path(test));
    TRY(dump_screenshot_to_file(actual, ByteString::formatted("{}.actual.png", base_path)));
    TRY(dump_screenshot_to_file(expected, ByteString::formatted("{}.expected.png", base_path)));

    // Generate a diff image and compute stats.
    if (actual.width() == expected.width() && actual.height() == expected.height()) {
        auto diff = actual.diff(expected);
        test.diff_pixel_error_count = diff.pixel_error_count;
        test.diff_maximum_error = diff.maximum_error;

        auto diff_bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { actual.width(), actual.height() }));
        for (int y = 0; y < actual.height(); ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                auto pixel = actual.get_pixel(x, y);
                if (pixel != expected.get_pixel(x, y))
                    diff_bitmap->set_pixel(x, y, Gfx::Color(255, 0, 0));
                else
                    diff_bitmap->set_pixel(x, y, pixel.mixed_with(expected.get_pixel(x, y), 0.5f).mixed_with(Gfx::Color::White, 0.8f));
            }
        }
        TRY(dump_screenshot_to_file(*diff_bitmap, ByteString::formatted("{}.diff.png", base_path)));
    }

    return {};
}

static void run_ref_test(TestWebView& view, TestRunContext& context, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    auto test_index = test.index;
    test.timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, test_index]() {
        view.on_test_complete({ test_index, TestResult::Timeout });
    });

    auto handle_completed_test = [&view, &context, test_index, url]() -> ErrorOr<TestResult> {
        auto& test = context.tests[test_index];
        VERIFY(test.ref_test_expectation_type.has_value());
        auto should_match = test.ref_test_expectation_type == RefTestExpectationType::Match;
        auto screenshot_matches = fuzzy_screenshot_match(url, view.url(), *test.actual_screenshot,
            *test.expectation_screenshot, test.fuzzy_matches, should_match);
        if (should_match == screenshot_matches)
            return TestResult::Pass;

        TRY(write_screenshot_failure_results(test, *test.actual_screenshot, *test.expectation_screenshot));
        return TestResult::Fail;
    };

    auto on_test_complete = [&view, test_index, handle_completed_test]() {
        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test_index, TestResult::Fail });
        else
            view.on_test_complete({ test_index, result.value() });
    };

    view.on_load_finish = [&view](auto const&) {
        view.run_javascript(wait_for_reftest_completion);
    };

    view.on_test_finish = [&view, &context, test_index, on_test_complete = move(on_test_complete)](auto const&) {
        auto& test = context.tests[test_index];
        if (test.actual_screenshot) {
            // The reference has finished loading; take another screenshot and move on to handling the result.
            view.take_screenshot()->when_resolved([&view, &context, test_index, on_test_complete = move(on_test_complete)](RefPtr<Gfx::Bitmap const> screenshot) {
                context.tests[test_index].expectation_screenshot = move(screenshot);
                view.reset_zoom();
                on_test_complete();
            });
        } else {
            // When the test initially finishes, we take a screenshot and request the reference test metadata.
            view.take_screenshot()->when_resolved([&view, &context, test_index](RefPtr<Gfx::Bitmap const> screenshot) {
                context.tests[test_index].actual_screenshot = move(screenshot);
                view.reset_zoom();
                view.run_javascript("internals.loadReferenceTestMetadata();"_string);
            });
        }
    };

    view.on_reference_test_metadata = [&view, &context, test_index](JsonValue const& metadata) {
        auto& test = context.tests[test_index];
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

    view.on_set_test_timeout = [&context, test_index, timeout_in_milliseconds](double milliseconds) {
        auto& test = context.tests[test_index];
        if (milliseconds > timeout_in_milliseconds)
            test.timeout_timer->restart(AK::clamp_to<int>(milliseconds));
    };

    view.load(url);
    test.timeout_timer->start();
}

static void run_screenshot_test(TestWebView& view, TestRunContext& context, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    auto test_index = test.index;
    test.timeout_timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, test_index]() {
        view.on_test_complete({ test_index, TestResult::Timeout });
    });

    auto handle_completed_test = [&context, test_index, url]() -> ErrorOr<TestResult> {
        auto& test = context.tests[test_index];
        auto& actual = *test.actual_screenshot;

        // Try to load and compare against existing expected PNG first.
        auto expectation_file_or_error = Core::MappedFile::map(test.expectation_path);
        if (!expectation_file_or_error.is_error()) {
            auto decoder = TRY(Gfx::ImageDecoder::try_create_for_raw_bytes(expectation_file_or_error.value()->bytes()));
            if (decoder) {
                auto frame = TRY(decoder->frame(0));
                test.expectation_screenshot = move(frame.image);

                auto const& expected = *test.expectation_screenshot;
                auto screenshot_matches = fuzzy_screenshot_match(url, url, actual, expected, test.fuzzy_matches, true);
                if (screenshot_matches)
                    return TestResult::Pass;
            }
        }

        // Screenshots don't match (or expected file doesn't exist yet).
        if (Application::the().rebaseline) {
            TRY(Core::Directory::create(LexicalPath { test.expectation_path }.parent().string(), Core::Directory::CreateDirectories::Yes));
            TRY(dump_screenshot_to_file(actual, test.expectation_path));
            return TestResult::Pass;
        }

        // Not rebaselining and no valid expectation loaded.
        if (!test.expectation_screenshot)
            return Error::from_string_literal("Could not decode expected screenshot PNG");

        TRY(write_screenshot_failure_results(test, actual, *test.expectation_screenshot));
        return TestResult::Fail;
    };

    auto on_test_complete = [&view, test_index, handle_completed_test]() {
        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test_index, TestResult::Fail });
        else
            view.on_test_complete({ test_index, result.value() });
    };

    view.on_load_finish = [&view](auto const&) {
        view.run_javascript(wait_for_reftest_completion);
    };

    view.on_test_finish = [&view, &context, test_index, on_test_complete = move(on_test_complete)](auto const&) {
        // Take a screenshot of the rendered test page.
        view.take_screenshot()->when_resolved([&view, &context, test_index, on_test_complete = move(on_test_complete)](RefPtr<Gfx::Bitmap const> screenshot) {
            context.tests[test_index].actual_screenshot = move(screenshot);
            view.reset_zoom();
            // Load reference test metadata for fuzzy matching config.
            view.run_javascript("internals.loadReferenceTestMetadata();"_string);

            view.on_reference_test_metadata = [&context, test_index, on_test_complete = move(on_test_complete)](JsonValue const& metadata) {
                auto& test = context.tests[test_index];
                auto metadata_object = metadata.as_object();

                // Read fuzzy configurations (ignore match/mismatch references for Screenshot tests).
                test.fuzzy_matches.clear_with_capacity();
                auto fuzzy_values = metadata_object.get_array("fuzzy"sv);
                for (size_t i = 0; i < fuzzy_values->size(); ++i) {
                    auto fuzzy_configuration = fuzzy_values->at(i).as_object();

                    auto content = fuzzy_configuration.get_string("content"sv).release_value();
                    auto fuzzy_match_or_error = parse_fuzzy_match({}, content);
                    if (fuzzy_match_or_error.is_error()) {
                        warnln("Failed to parse fuzzy configuration '{}': {}", content, fuzzy_match_or_error.error());
                        continue;
                    }

                    test.fuzzy_matches.append(fuzzy_match_or_error.release_value());
                }

                on_test_complete();
            };
        });
    };

    view.on_set_test_timeout = [&context, test_index, timeout_in_milliseconds](double milliseconds) {
        auto& test = context.tests[test_index];
        if (milliseconds > timeout_in_milliseconds)
            test.timeout_timer->restart(milliseconds);
    };

    view.load(url);
    test.timeout_timer->start();
}

static void run_test(TestWebView& view, TestRunContext& context, size_t test_index, Application& app)
{
    s_current_test_index_by_view.set(&view, test_index);

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

    promise->when_resolved([&view, test_index, &app, &context](auto) {
        auto& test = context.tests[test_index];
        auto real_path = MUST(FileSystem::real_path(test.input_path));
        auto headers_path = ByteString::formatted("{}.headers", real_path);

        Optional<URL::URL> url;
        if (FileSystem::exists(headers_path) || s_loaded_from_http_server.contains_slow(test.input_path)) {
            // Some tests need to be served via the echo server so, for example, HTTP headers from .headers files are
            // sent, or so that the resulting HTML document has a HTTP based origin (e.g for testing cookies).
            auto echo_server_port = Application::web_content_options().echo_server_port;
            VERIFY(echo_server_port.has_value());
            auto relative_path = LexicalPath::relative_path(real_path, app.test_root_path);
            VERIFY(relative_path.has_value());
            url = URL::Parser::basic_parse(ByteString::formatted("http://localhost:{}/static/{}", echo_server_port.value(), relative_path.value())).release_value();
        } else {
            url = URL::create_with_file_scheme(real_path).release_value();
        }

        // Append variant query string if present (variant is "?foo=bar", set_query expects "foo=bar")
        if (test.variant.has_value())
            url->set_query(MUST(test.variant->substring_from_byte_offset_with_shared_superstring(1)));

        switch (test.mode) {
        case TestMode::Crash:
        case TestMode::Text:
        case TestMode::Layout:
            run_dump_test(view, context, test, *url, app.per_test_timeout_in_seconds * 1000);
            return;
        case TestMode::Ref:
            run_ref_test(view, context, test, *url, app.per_test_timeout_in_seconds * 1000);
            return;
        case TestMode::Screenshot:
            run_screenshot_test(view, context, test, *url, app.per_test_timeout_in_seconds * 1000);
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
        if (auto index = s_current_test_index_by_view.get(&view); index.has_value()) {
            if (s_run_context) {
                auto& capture = output_capture_for_view(view);
                (void)write_output_for_test(s_run_context->tests[*index], capture);
            }
        }

        // Re-setup output capture for the respawned WebContent process
        // (handle_web_content_process_crash already ran and respawned it)
        s_output_captures.remove(&view);
        setup_output_capture_for_view(view);

        if (auto index = s_current_test_index_by_view.get(&view); index.has_value()) {
            view.on_test_complete({ *index, TestResult::Crashed });
        }
    };

    view.on_web_content_process_change_for_cross_site_navigation = [&view]() {
        s_output_captures.remove(&view);
        setup_output_capture_for_view(view);
    };
}

static ErrorOr<int> run_tests(Core::AnonymousBuffer const& theme, Web::DevicePixelSize window_size)
{
    auto& app = Application::the();
    TRY(load_test_config(app.test_root_path));

    Vector<Test> tests;

    // Parse explicit variants from filters (e.g., "test.html?variant=foo")
    HashMap<ByteString, String> explicit_variants;
    for (auto& glob : app.test_globs) {
        if (auto query_pos = glob.find('?'); query_pos.has_value()) {
            auto base_glob = glob.substring(0, query_pos.value());
            auto variant = MUST(String::from_utf8(glob.substring_view(query_pos.value())));
            explicit_variants.set(ByteString::formatted("*{}*", base_glob), variant);
            glob = ByteString::formatted("*{}*", base_glob);
        } else {
            glob = ByteString::formatted("*{}*", glob);
        }
    }
    if (app.test_globs.is_empty())
        app.test_globs.append("*"sv);

    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Layout", app.test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Text", app.test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(app, tests, ByteString::formatted("{}/Ref", app.test_root_path), "."sv));
    TRY(collect_crash_tests(app, tests, ByteString::formatted("{}/Crash", app.test_root_path), "."sv));
    TRY(collect_screenshot_tests(app, tests, ByteString::formatted("{}/Screenshot", app.test_root_path), "."sv));

    tests.remove_all_matching([&](auto const& test) {
        static constexpr Array support_file_patterns {
            "*/wpt-import/*/support/*"sv,
            "*/wpt-import/*/resources/*"sv,
            "*/wpt-import/common/*"sv,
            "*/wpt-import/images/*"sv,
        };
        auto normalize_path = [](ByteString const& path) { return path.replace("\\"sv, "/"sv); };
        auto const test_input_path = normalize_path(test.input_path);
        auto const test_relative_path = normalize_path(test.relative_path);
        bool is_support_file = any_of(support_file_patterns, [&](auto pattern) { return test_input_path.matches(pattern); });
        bool match_glob = any_of(app.test_globs, [&](auto const& glob) { return test_relative_path.matches(glob, CaseSensitivity::CaseSensitive); });
        return is_support_file || !match_glob;
    });

    // Apply explicit variants from filters
    for (auto& test : tests) {
        for (auto const& [glob, variant] : explicit_variants) {
            if (test.relative_path.matches(glob, CaseSensitivity::CaseSensitive)) {
                test.variant = variant;
                auto variant_suffix = variant.bytes_as_string_view().substring_view(1);
                test.relative_path = ByteString::formatted("{}?{}", test.relative_path, variant_suffix);
                test.safe_relative_path = ByteString::formatted("{}@{}", test.safe_relative_path, variant_suffix);
                auto dir = LexicalPath::dirname(test.expectation_path);
                auto title = LexicalPath::title(LexicalPath::basename(test.input_path));
                if (dir.is_empty())
                    test.expectation_path = ByteString::formatted("{}@{}.txt", title, variant_suffix);
                else
                    test.expectation_path = ByteString::formatted("{}/{}@{}.txt", dir, title, variant_suffix);
                break;
            }
        }
    }

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

    s_current_run = 1;

    if (app.repeat_count > 1) {
        auto base_tests = move(tests);
        tests.ensure_capacity(base_tests.size() * app.repeat_count);

        for (size_t run_index = 1; run_index <= app.repeat_count; ++run_index) {
            for (auto const& base_test : base_tests) {
                Test test = base_test;
                test.run_index = run_index;
                test.total_runs = app.repeat_count;
                test.safe_relative_path = LexicalPath::join(ByteString::formatted("run-{}", run_index), test.safe_relative_path).string();
                tests.append(move(test));
            }
        }
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

    // Initialize view display states (used for idle tracking even when not on TTY)
    s_view_display_states.resize(concurrency);
    for (auto [i, view] : enumerate(views)) {
        s_view_display_states[i].pid = view->web_content_pid();
        s_view_display_states[i].active = false;
    }

    // Initialize per-view functions (for waking idle views)
    s_view_run_next_test.resize_and_keep_capacity(concurrency);

    // Initialize live terminal display
    s_is_tty = TRY(Core::System::isatty(STDOUT_FILENO));

    // When on TTY with live display, use the N-line display; otherwise use single-line or verbose
    bool use_live_display = s_is_tty && app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT;

    if (use_live_display) {
        update_terminal_size();

#ifndef AK_OS_WINDOWS
        // Handle terminal resize
        Core::EventLoop::register_signal(SIGWINCH, [](int) {
            Core::EventLoop::current().deferred_invoke([] {
                update_terminal_size();
            });
        });
#endif

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
    s_fail_fast_triggered = false;

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

    TestRunContext context { tests, tests_remaining, s_total_tests };
    s_run_context = &context;
    ScopeGuard clear_run_context = [&] { s_run_context = nullptr; };

    Vector<TestCompletion> non_passing_tests;

    auto digits_for_view_id = static_cast<size_t>(log10(views.size()) + 1);
    auto digits_for_test_id = static_cast<size_t>(log10(tests.size()) + 1);

    for (auto [view_id, view] : enumerate(views)) {
        set_ui_callbacks_for_tests(*view);
        view->clear_content_filters();

        auto cleanup_test = [&, view = view.ptr()](size_t test_index, TestResult test_result) {
            view->on_load_finish = {};
            view->on_test_finish = {};
            view->on_reference_test_metadata = {};
            view->on_test_variant_metadata = {};
            view->on_set_test_timeout = {};

            // Disconnect child crash handlers so old child crashes don't affect the next test
            view->disconnect_child_crash_handlers();

            // Don't try to reset state if WebContent crashed - it's gone
            if (test_result != TestResult::Crashed) {
                view->reset_zoom();
                view->reset_viewport_size(window_size);
            }

            auto& test = tests[test_index];
            if (test.timeout_timer) {
                test.timeout_timer->stop();
                test.timeout_timer.clear();
            }

            s_current_test_index_by_view.remove(view);
        };

        // run_next_test handles: reset promise, attach callback, pick test, run test
        auto run_next_test = [&, view = view.ptr(), cleanup_test, view_id]() {
            if (app.fail_fast && s_fail_fast_triggered) {
                if (view_id < s_view_display_states.size())
                    s_view_display_states[view_id].active = false;
                return;
            }

            // Check without incrementing first - only consume an index if we have a test
            if (current_test >= tests.size()) {
                // Mark this view as idle (for variant wake-up tracking)
                if (view_id < s_view_display_states.size())
                    s_view_display_states[view_id].active = false;
                return;
            }
            auto index = current_test++;

            auto& test = tests[index];
            s_current_run = test.run_index;
            test.start_time = UnixDateTime::now();
            test.index = index;

            // Mark this view as active (for variant wake-up tracking)
            if (view_id < s_view_display_states.size())
                s_view_display_states[view_id].active = true;

            if (use_live_display) {
                // Update view display state for live display (refresh PID in case WebContent respawned)
                if (view_id < s_view_display_states.size()) {
                    s_view_display_states[view_id].pid = view->web_content_pid();
                    s_view_display_states[view_id].test_name = test.relative_path;
                    s_view_display_states[view_id].start_time = test.start_time;
                }
                render_live_display();
            } else if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_DURATION) {
                outln("[{:{}}] {:{}}/{}:  Start {}", view_id, digits_for_view_id, test.index + 1, digits_for_test_id, tests.size(), test.relative_path);
            } else {
                // Non-TTY mode: print each test as it starts
                outln("{}/{}: {}", test.index + 1, tests.size(), test.relative_path);
            }

            // Reset promise and attach completion callback
            view->reset_test_promise();
            view->test_promise().when_resolved([&tests, &tests_remaining, &non_passing_tests, &app, view, cleanup_test, view_id, digits_for_view_id, digits_for_test_id, use_live_display](auto result) {
                cleanup_test(result.test_index, result.result);

                auto& test = tests[result.test_index];

                // Clear screenshots to free memory
                test.actual_screenshot.clear();
                test.expectation_screenshot.clear();

                test.end_time = UnixDateTime::now();

                if (result.result == TestResult::Timeout && app.debug_timeouts) {
                    auto& capture = output_capture_for_view(*view);
                    StringBuilder diagnostics;
                    append_timeout_diagnostics_to_stderr(diagnostics, *view, test, view_id);
                    auto diagnostics_view = diagnostics.string_view();
                    capture.stderr_buffer.append(diagnostics_view);

                    if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                        (void)Core::System::write(STDERR_FILENO, diagnostics_view.bytes());
                }

                // Write captured stdout/stderr to results directory.
                // NOTE: On crashes, we already flushed it in on_web_content_crashed.
                if (result.result != TestResult::Crashed) {
                    auto& capture = output_capture_for_view(*view);
                    (void)write_output_for_test(test, capture);
                }

                if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_DURATION) {
                    auto duration = test.end_time - test.start_time;
                    outln("[{:{}}] {:{}}/{}: Finish {}: {}ms", view_id, digits_for_view_id, test.index + 1, digits_for_test_id, tests.size(), test.relative_path, duration.to_milliseconds());
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
                case TestResult::Expanded:
                    break;
                }

                // Don't count Expanded tests in the completed display count
                if (result.result != TestResult::Expanded)
                    ++s_completed_tests;

                bool const is_non_passing_result = result.result != TestResult::Pass && result.result != TestResult::Expanded;
                bool const should_trigger_fail_fast = result.result == TestResult::Fail || result.result == TestResult::Timeout || result.result == TestResult::Crashed;

                if (is_non_passing_result)
                    non_passing_tests.append(result);

                if (app.fail_fast && !s_fail_fast_triggered && should_trigger_fail_fast) {
                    s_fail_fast_triggered = true;

                    if (s_display_timer) {
                        s_display_timer->stop();
                        s_display_timer = nullptr;
                    }

                    if (use_live_display) {
                        for (size_t i = 0; i < s_live_display_lines; ++i)
                            out("\033[A\033[2K"sv);
                        out("\r"sv);
                        (void)fflush(stdout);
                        s_live_display_lines = 0;
                        print_deferred_warnings();
                    }

                    auto const pid = view->web_content_pid();
                    if (result.result == TestResult::Timeout)
                        outln("Fail-fast: Timeout: {} (pid {})", test.relative_path, pid);
                    else
                        outln("Fail-fast: {}: {}", test_result_to_string(result.result), test.relative_path);

                    if (result.result == TestResult::Timeout) {
                        auto& capture = output_capture_for_view(*view);
                        StringBuilder backtrace_output;
                        append_timeout_backtraces_to_stderr(backtrace_output, *view, test, view_id);
                        auto backtrace_output_view = backtrace_output.string_view();
                        capture.stderr_buffer.append(backtrace_output_view);

                        if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
                            (void)Core::System::write(STDERR_FILENO, backtrace_output_view.bytes());
                    }

                    if (s_all_tests_complete)
                        s_all_tests_complete->reject(Error::from_string_literal("Fail-fast"));
                    Core::EventLoop::current().quit(1);

                    if (result.result == TestResult::Timeout)
                        maybe_attach_on_fail_fast_timeout(pid);

                    return;
                }

                if (--tests_remaining == 0) {
                    s_all_tests_complete->resolve({});
                } else {
                    // Use deferred_invoke to avoid destroying callback while inside it
                    Core::deferred_invoke([view_id]() {
                        // Wake any idle views to help with remaining tests
                        for (size_t i = 0; i < s_view_run_next_test.size(); ++i) {
                            if (i < s_view_display_states.size() && !s_view_display_states[i].active && s_view_run_next_test[i])
                                s_view_run_next_test[i]();
                        }
                        // Run next test for this view
                        s_view_run_next_test[view_id]();
                    });
                }
            });

            Core::deferred_invoke([&, index]() mutable {
                if (s_skipped_tests.contains_slow(tests[index].input_path))
                    view->on_test_complete({ index, TestResult::Skipped });
                else
                    run_test(*view, context, index, app);
            });
        };

        // Store in static vector for access by variant expansion wake-up
        s_view_run_next_test[view_id] = move(run_next_test);

        Core::deferred_invoke([view_id]() {
            s_view_run_next_test[view_id]();
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

        auto const& test = tests[non_passing_test.test_index];
        if (Application::the().repeat_count > 1)
            outln("{}: (run {}/{}) {}", test_result_to_string(non_passing_test.result), test.run_index, test.total_runs, test.relative_path);
        else
            outln("{}: {}", test_result_to_string(non_passing_test.result), test.relative_path);
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
            if (auto path = view->dump_gc_graph(); path.is_error()) {
                warnln("Failed to dump GC graph: {}", path.error());
            } else {
                outln("GC graph dumped to {}", path.value());
                auto source_root = LexicalPath(app.test_root_path).parent().parent().string();
                outln("GC graph explorer: file://{}/Meta/gc-heap-explorer.html?script=file://{}", source_root, path.value());
            }
        }
    }

    // Generate result files (JSON data and HTML index)
    if (app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT || !non_passing_tests.is_empty()) {
        if (auto result = generate_result_files(tests, non_passing_tests); result.is_error())
            warnln("Failed to generate result files: {}", result.error());
        else
            outln("Results: file://{}/index.html", app.results_directory);
    }

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

        auto maybe_index = s_current_test_index_by_view.get(&view);
        if (maybe_index.has_value() && s_run_context) {
            auto const& test = s_run_context->tests[*maybe_index];
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

    if (app->repeat_count > 1 && app->rebaseline) {
        warnln("Error: --repeat cannot be used together with --rebaseline.");
        warnln("Run once with --rebaseline, or drop --rebaseline when repeating.");
        return 1;
    }
    Core::EventLoop::register_signal(SIGINT, TestWeb::handle_signal);
    Core::EventLoop::register_signal(SIGTERM, TestWeb::handle_signal);

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto const& browser_options = TestWeb::Application::browser_options();
    Web::DevicePixelSize window_size { browser_options.window_width, browser_options.window_height };

    VERIFY(!app->test_root_path.is_empty());

    app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);

    app->results_directory = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->results_directory);
    TRY(Core::Directory::create(app->results_directory, Core::Directory::CreateDirectories::Yes));

    TRY(app->launch_test_fixtures());

    return TestWeb::run_tests(theme, window_size);
}
