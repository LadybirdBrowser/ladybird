/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Display.h"
#include "Application.h"

#include <AK/Enumerate.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <AK/SaturatingMath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibGfx/Size.h>

#ifndef AK_OS_WINDOWS
#    include <sys/ioctl.h>
#endif

namespace TestWeb {

static constexpr size_t LIVE_DISPLAY_TERMINAL_HEADROOM = 4; // allow for external cruft like tmux panels
static constexpr size_t LIVE_DISPLAY_STATUS_LINES = 4;      // 2 empty + 1 for status + 1 for progress bar
static size_t s_display_rows = 80;
static size_t s_display_columns = 24;
static size_t count_digits(size_t value);

Display& Display::the()
{
    static Display instance;
    return instance;
}

void Display::begin_run()
{
    auto& app = Application::the();
    is_tty = Core::System::isatty(STDOUT_FILENO).value_or(false);
    is_live_display_active = !app.quiet && is_tty && app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT;

    outln("Running {} tests...", total_tests());

    if (!is_live_display_active)
        return;

#ifndef AK_OS_WINDOWS
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        s_display_rows = ws.ws_row > 0 ? ws.ws_row : 24;
        s_display_columns = ws.ws_col > 0 ? ws.ws_col : 80;
    }
#endif
    s_display_rows = AK::clamp(
        AK::saturating_sub(s_display_rows, LIVE_DISPLAY_TERMINAL_HEADROOM),
        LIVE_DISPLAY_STATUS_LINES + 1,
        view_states().size() + LIVE_DISPLAY_STATUS_LINES);

    display_timer = Core::Timer::create_repeating(1000, [this] { render_live_display(); });
    display_timer->start();

    for (size_t i = 0; i < s_display_rows; i++) {
        outln();
    }
    (void)fflush(stdout);
}

void Display::on_test_started(size_t view_index, Test const& test, pid_t pid)
{
    current_run = test.run_index;
    if (view_index < view_states().size()) {
        auto& state = view_states()[view_index];
        state.pid = pid;
        state.test_name = test.relative_path;
        state.start_time = test.start_time;
        state.active = true;
    }
    if (is_live_display_active) {
        render_live_display();
        return;
    }
    if (Application::the().quiet)
        return;

    if (Application::the().verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_DURATION) {
        outln("[{:{}}] {:{}}/{}:  Start {}", view_index, count_digits(view_states().size()), test.index + 1,
            count_digits(total_tests()), total_tests(), test.relative_path);
        return;
    }
    outln("{}/{}: {}", test.index + 1, total_tests(), test.relative_path);
}

void Display::on_test_finished(size_t view_index, Test const& test, TestResult result)
{
    switch (result) {
    case TestResult::Pass:
        ++pass_count;
        break;
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
    case TestResult::Expanded:
        break;
    }
    if (result != TestResult::Expanded)
        ++completed_tests;

    if (view_index >= view_states().size())
        return;

    auto& app = Application::the();
    if (app.quiet || app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_DURATION)
        return;

    auto duration = test.end_time - test.start_time;
    outln("[{:{}}] {:{}}/{}: Finish {}: {}ms", view_index, count_digits(view_states().size()), test.index + 1,
        count_digits(total_tests()), total_tests(), test.relative_path, duration.to_milliseconds());
}

void Display::on_fail_fast(Test const& test, TestResult result, pid_t pid)
{
    clear_live_display();

    if (result == TestResult::Timeout)
        outln("Fail-fast: Timeout: {} (pid {})", test.relative_path, pid);
    else
        outln("Fail-fast: {}: {}", test_result_to_string(result), test.relative_path);
}

void Display::print_run_complete(ReadonlySpan<Test> tests,
    ReadonlySpan<TestCompletion> non_passing_tests,
    size_t tests_remaining) const
{
    if (tests_remaining > 0)
        outln("Halted; {} tests not executed.", tests_remaining);

    outln("==========================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}, Crashed: {}", pass_count, fail_count, skipped_count,
        timeout_count, crashed_count);
    outln("==========================================================");

    auto& app = Application::the();
    for (auto const& non_passing_test : non_passing_tests) {
        if (non_passing_test.result == TestResult::Skipped && app.verbosity < Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS)
            continue;

        auto const& test = tests[non_passing_test.test_index];
        if (app.repeat_count > 1)
            outln("{}: (run {}/{}) {}", test_result_to_string(non_passing_test.result), test.run_index,
                test.total_runs, test.relative_path);
        else
            outln("{}: {}", test_result_to_string(non_passing_test.result), test.relative_path);
    }

    if (!app.quiet && app.verbosity >= Application::VERBOSITY_LEVEL_LOG_SLOWEST_TESTS) {
        auto tests_to_print = min(10uz, tests.size());
        outln("\nSlowest {} tests:", tests_to_print);

        Vector<Test const*> sorted_tests;
        sorted_tests.ensure_capacity(tests.size());
        for (auto const& test : tests)
            sorted_tests.unchecked_append(&test);

        quick_sort(sorted_tests, [](Test const* lhs, Test const* rhs) {
            auto lhs_duration = lhs->end_time - lhs->start_time;
            auto rhs_duration = rhs->end_time - rhs->start_time;
            return lhs_duration > rhs_duration;
        });
        for (auto const* test : sorted_tests.span().trim(tests_to_print)) {
            auto duration = test->end_time - test->start_time;
            outln("{}: {}ms", test->relative_path, duration.to_milliseconds());
        }
    }
}

void Display::print_failure_diff(URL::URL const& url, Test const& test,
    ByteBuffer const& expectation) const
{
    auto& app = Application::the();
    if (app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT)
        return;

    auto const color_output = is_tty ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;
    if (color_output == Diff::ColorOutput::Yes)
        outln("\n\033[33;1mTest failed\033[0m: {}", url);
    else
        outln("\nTest failed: {}", url);

    auto maybe_hunks = Diff::from_text(expectation, test.text, 3);
    if (maybe_hunks.is_error()) {
        outln("Failed to generate diff: {}", maybe_hunks.error());
        return;
    }
    auto const& hunks = maybe_hunks.release_value();
    auto out = MUST(Core::File::standard_output());

    (void)Diff::write_unified_header(test.expectation_path, test.expectation_path, *out);
    for (auto const& hunk : hunks)
        (void)Diff::write_unified(hunk, *out, color_output);
}

void Display::render_live_display() const
{
    if (!is_tty || !is_live_display_active)
        return;

    auto now = UnixDateTime::now();
    StringBuilder output;

    for (size_t i = 0; i < s_display_rows; ++i)
        output.append("\033[A"sv);
    output.append("\r"sv);

    size_t num_view_lines = s_display_rows - LIVE_DISPLAY_STATUS_LINES;
    if (num_view_lines > 1 && num_view_lines < view_states().size())
        num_view_lines--; // "Hidden views" line

    for (size_t i = 0; i < num_view_lines; ++i) {
        output.append("\033[2K"sv);

        if (i < view_states().size()) {
            auto const& state = view_states()[i];
            if (state.active && state.pid > 0) {
                auto duration = (now - state.start_time).to_truncated_seconds();
                auto prefix = ByteString::formatted("\033[33m⏺\033[0m {} ({}s): ", state.pid, duration);
                size_t const prefix_visible_length = ByteString::formatted("⏺ {} ({}s): ", state.pid, duration).length();
                size_t const available_width = s_display_columns > prefix_visible_length
                    ? s_display_columns - prefix_visible_length
                    : 10;
                ByteString name = state.test_name;
                if (name.length() > available_width && available_width > 3)
                    name = ByteString::formatted("...{}", name.substring_view(name.length() - available_width + 3));

                output.appendff("{}{}", prefix, name);
            } else {
                output.append("\033[90m⏺ (idle)\033[0m"sv);
            }
        }
        output.append("\n"sv);
    }
    if (num_view_lines < view_states().size()) {
        output.append("\033[2K\033[90m... "sv);
        output.appendff("{} more views hidden\033[0m", view_states().size() - num_view_lines);
        output.append("\n"sv);
    }
    output.append("\033[2K\n\033[2K"sv);
    output.appendff("\033[1;32mPass:\033[0m {}, ", pass_count);
    output.appendff("\033[1;31mFail:\033[0m {}, ", fail_count);
    output.appendff("\033[1;90mSkipped:\033[0m {}, ", skipped_count);
    output.appendff("\033[1;33mTimeout:\033[0m {}, ", timeout_count);
    output.appendff("\033[1;35mCrashed:\033[0m {}", crashed_count);
    output.append("\n\033[2K\n\033[2K"sv);

    if (total_tests() > 0) {
        auto counter_start = output.length();
        output.appendff("{}/{} ", completed_tests, total_tests());
        if (Application::the().repeat_count > 1)
            output.appendff("run {}/{} ", current_run, Application::the().repeat_count);

        auto const counter_length = output.length() - counter_start;
        size_t const bar_width = s_display_columns > counter_length + 3 ? s_display_columns - counter_length - 3 : 20;
        size_t const filled = (completed_tests * bar_width) / total_tests();
        size_t const empty = bar_width - filled;

        output.append("\033[32m["sv);
        for (size_t j = 0; j < filled; ++j) {
            output.append("█"sv);
        }
        if (empty > 0 && filled < bar_width) {
            output.append("\033[33m▓\033[0m\033[90m"sv);
            for (size_t j = 1; j < empty; ++j) {
                output.append("░"sv);
            }
        }
        output.append("\033[32m]\033[0m"sv);
    }
    output.append("\n"sv);

    out("{}", output.string_view());
    (void)fflush(stdout);
}

void Display::clear_live_display()
{
    if (!is_live_display_active)
        return;
    if (display_timer) {
        display_timer->stop();
        display_timer = nullptr;
    }
    for (size_t i = 0; i < s_display_rows; ++i) {
        out("\033[A\033[2K"sv);
    }
    out("\r"sv);
    (void)fflush(stdout);

    is_live_display_active = false;
}

static size_t count_digits(size_t value)
{
    size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

} // namespace TestWeb
