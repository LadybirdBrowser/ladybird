/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportDiagnostics.h>

using WebView::CrashReportDiagnostics;

static constexpr auto stack_heading = "Native stack (binary build ID, object address):"sv;

TEST_CASE(named_lines_are_read_in_order)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Process: WebContent\n"
                                                     "Version: 1.0\n"
                                                     "Platform: macOS\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 3u);
    EXPECT_EQ(diagnostics.headers[0].name, "Process"sv);
    EXPECT_EQ(diagnostics.headers[0].value, "WebContent"sv);
    EXPECT_EQ(diagnostics.headers[2].name, "Platform"sv);
    EXPECT_EQ(diagnostics.header("Version"sv), "1.0"sv);

    // The first line names the format rather than a field.
    EXPECT_EQ(diagnostics.header("Ladybird crash report, format 1"sv), ""sv);
    EXPECT_EQ(diagnostics.header("Architecture"sv), ""sv);
}

TEST_CASE(a_value_may_contain_colons)
{
    auto diagnostics = CrashReportDiagnostics::parse(
        "Ladybird crash report, format 1\n"
        "Verification failed: value != 0 at Services/WebContent/Foo.cpp:123\n"sv);

    EXPECT_EQ(diagnostics.header("Verification failed"sv), "value != 0 at Services/WebContent/Foo.cpp:123"sv);
}

TEST_CASE(a_wrapped_value_belongs_to_the_field_above_it)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Build options: first\n"
                                                     "    second\n"
                                                     "Process: WebContent\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 2u);
    EXPECT_EQ(diagnostics.header("Build options"sv), "first\n    second"sv);
    EXPECT_EQ(diagnostics.header("Process"sv), "WebContent"sv);
}

TEST_CASE(a_signal_has_a_name_and_a_number)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal: SIGSEGV\n"
                                                     "Termination signal number: 11\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGSEGV"sv);
    EXPECT_EQ(diagnostics.signal.number, 11u);
}

TEST_CASE(a_signal_without_a_readable_number_still_has_its_name)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal: SIGSEGV\n"
                                                     "Termination signal number: not a number\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGSEGV"sv);
    EXPECT(!diagnostics.signal.number.has_value());
}

TEST_CASE(a_captured_signal_is_used_when_the_report_has_no_termination_signal)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Exit code: 3\n"
                                                     "Captured signal: SIGABRT\n"
                                                     "Captured signal number: 6\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGABRT"sv);
    EXPECT_EQ(diagnostics.signal.number, 6u);
}

TEST_CASE(a_termination_signal_is_preferred_over_a_captured_one)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal: SIGSEGV\n"
                                                     "Termination signal number: 11\n"
                                                     "Captured signal: SIGABRT\n"
                                                     "Captured signal number: 6\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGSEGV"sv);
    EXPECT_EQ(diagnostics.signal.number, 11u);
}

TEST_CASE(a_line_below_the_stack_heading_is_part_of_the_stack_and_not_a_field)
{
    auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\n"
                                                                           "Process: WebContent\n"
                                                                           "\n{}\n"
                                                                           "#0 a1b2c3 0x1000 first_symbol + 0x10\n"
                                                                           "#1 d4e5f6 0x2000\n",
        stack_heading));

    EXPECT_EQ(diagnostics.headers.size(), 1u);
    EXPECT(diagnostics.stack.contains("#1 d4e5f6 0x2000"sv));
}

TEST_CASE(the_time_of_the_crash_is_read_from_the_report)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Crashed at: 2026-10-01T12:34:56Z\n"sv);

    EXPECT_EQ(diagnostics.crashed_at, UnixDateTime::from_unix_time_parts(2026, 10, 1, 12, 34, 56, 0));
}

TEST_CASE(a_time_of_the_crash_that_cannot_be_read_is_left_out)
{
    for (auto value : { ""sv, "yesterday"sv, "2026-10-01 12:34:56"sv, "2026-10-01T12:34:56"sv, "2026-13-01T12:34:56Z"sv,
             "2026-10-32T12:34:56Z"sv, "2026-10-01T24:00:00Z"sv, "2026-10-01T12:60:00Z"sv, "2026-10-01T12:34:60Z"sv,
             "2026-10-01T12:34:5xZ"sv }) {
        auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\nCrashed at: {}\n", value));
        EXPECT(!diagnostics.crashed_at.has_value());
    }

    EXPECT(!CrashReportDiagnostics::parse("Ladybird crash report, format 1\nProcess: WebContent\n"sv).crashed_at.has_value());
}

TEST_CASE(the_stack_keeps_its_heading_and_drops_the_note_about_partial_stacks)
{
    auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\n"
                                                                           "Process: WebContent\n"
                                                                           "\n{}\n"
                                                                           "#0 a1b2c3 0x1000 symbol\n"
                                                                           "\nStacks may be partial.\n",
        stack_heading));

    EXPECT(diagnostics.stack.starts_with(stack_heading));
    EXPECT(diagnostics.stack.contains("#0 a1b2c3 0x1000 symbol"sv));
    EXPECT(!diagnostics.stack.contains("Stacks may be partial."sv));
    EXPECT(!diagnostics.stack.contains("Process: WebContent"sv));
}

TEST_CASE(a_report_without_a_stack_section_still_reads_its_fields)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Process: WebContent\n"
                                                     "Unavailable: the process exited without a captured native stack.\n"sv);

    EXPECT_EQ(diagnostics.header("Process"sv), "WebContent"sv);
    EXPECT(diagnostics.stack.is_empty());
}

TEST_CASE(an_empty_report_parses_to_nothing)
{
    auto diagnostics = CrashReportDiagnostics::parse(""sv);
    EXPECT(diagnostics.headers.is_empty());
    EXPECT(diagnostics.stack.is_empty());
    EXPECT_EQ(diagnostics.signal.name, ""sv);
}
