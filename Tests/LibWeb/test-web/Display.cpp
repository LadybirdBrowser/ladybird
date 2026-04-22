/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Display.h"
#include "Application.h"

#include <AK/Enumerate.h>
#include <AK/QuickSort.h>
#include <AK/SaturatingMath.h>
#include <AK/StringBuilder.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibTest/LiveDisplay.h>

#ifndef AK_OS_WINDOWS
#    include <signal.h>
#    include <sys/ioctl.h>
#    include <unistd.h>
#endif

namespace TestWeb {

static constexpr size_t LIVE_DISPLAY_TERMINAL_HEADROOM = 4; // allow for external cruft like tmux panels
static constexpr size_t LIVE_DISPLAY_STATUS_LINES = 4;      // 2 empty + 1 for status + 1 for progress bar
static size_t s_display_rows = 24;

static ::Test::LiveDisplay s_live_display;

static size_t count_digits(size_t value);

Display& Display::the()
{
    static Display instance;
    return instance;
}

void Display::begin_run()
{
    auto& app = Application::the();
    is_tty = ::Test::stdout_is_tty();
    bool const want_live_display = !app.quiet && is_tty && app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_OUTPUT;

    outln("Running {} tests...", total_tests());

    if (!want_live_display)
        return;

    size_t terminal_rows = 24;
#ifndef AK_OS_WINDOWS
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        terminal_rows = ws.ws_row;
#endif
    s_display_rows = AK::clamp(
        AK::saturating_sub(terminal_rows, LIVE_DISPLAY_TERMINAL_HEADROOM),
        LIVE_DISPLAY_STATUS_LINES + 1,
        view_states().size() + LIVE_DISPLAY_STATUS_LINES);

    is_live_display_active = s_live_display.begin({ .reserved_lines = s_display_rows, .log_file_path = {} });
    if (!is_live_display_active)
        return;

#ifndef AK_OS_WINDOWS
    Core::EventLoop::register_signal(SIGWINCH, [](int) {
        Core::EventLoop::current().deferred_invoke([] {
            s_live_display.refresh_terminal_width();
        });
    });
#endif

    display_timer = Core::Timer::create_repeating(1000, [this] { render_live_display(); });
    display_timer->start();
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
    if (!s_live_display.is_active())
        return;

    auto now = UnixDateTime::now();

    s_live_display.render([&](::Test::LiveDisplay::RenderTarget& t) {
        size_t const reserved = s_live_display.reserved_lines();
        size_t num_view_lines = reserved > LIVE_DISPLAY_STATUS_LINES ? reserved - LIVE_DISPLAY_STATUS_LINES : 0;
        bool const need_hidden_line = num_view_lines > 1 && num_view_lines < view_states().size();
        if (need_hidden_line)
            num_view_lines--;

        for (size_t i = 0; i < num_view_lines; ++i) {
            t.line([&] {
                if (i >= view_states().size())
                    return;
                auto const& state = view_states()[i];
                if (state.active && state.pid > 0) {
                    auto duration = (now - state.start_time).to_truncated_seconds();
                    auto prefix = ByteString::formatted("⏺ {} ({}s): ", state.pid, duration);
                    t.label(prefix, state.test_name);
                } else {
                    t.label("⏺ (idle)"sv, {}, { .prefix = ::Test::LiveDisplay::Gray, .text = ::Test::LiveDisplay::None });
                }
            });
        }
        if (need_hidden_line) {
            t.line([&] {
                auto label = ByteString::formatted("... {} more views hidden", view_states().size() - num_view_lines);
                t.label(label, {}, { .prefix = ::Test::LiveDisplay::Gray, .text = ::Test::LiveDisplay::None });
            });
        }

        t.lines(
            [] {},
            [&] {
                t.counter({
                    { .label = "Pass"sv, .color = ::Test::LiveDisplay::Green, .value = pass_count },
                    { .label = "Fail"sv, .color = ::Test::LiveDisplay::Red, .value = fail_count },
                    { .label = "Skipped"sv, .color = ::Test::LiveDisplay::Gray, .value = skipped_count },
                    { .label = "Timeout"sv, .color = ::Test::LiveDisplay::Yellow, .value = timeout_count },
                    { .label = "Crashed"sv, .color = ::Test::LiveDisplay::Magenta, .value = crashed_count },
                });
            },
            [] {},
            [&] {
                if (total_tests() == 0)
                    return;
                ByteString suffix;
                if (Application::the().repeat_count > 1)
                    suffix = ByteString::formatted("run {}/{}", current_run, Application::the().repeat_count);
                t.progress_bar(completed_tests, total_tests(), suffix);
            });
    });
}

void Display::clear_live_display()
{
    if (!is_live_display_active)
        return;
    if (display_timer) {
        display_timer->stop();
        display_timer = nullptr;
    }
    s_live_display.end();
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
