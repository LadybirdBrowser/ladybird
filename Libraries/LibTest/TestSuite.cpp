/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/Time.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibTest/Macros.h>
#include <LibTest/TestResult.h>
#include <LibTest/TestSuite.h>

namespace Test {

TestSuite* TestSuite::s_global = nullptr;

class TestElapsedTimer {
public:
    TestElapsedTimer() { restart(); }

    void restart() { m_started = UnixDateTime::now(); }

    AK::Duration elapsed() const
    {
        return UnixDateTime::now() - m_started;
    }

private:
    UnixDateTime m_started;
};

// Declared in Macros.h
TestResult current_test_result()
{
    return TestSuite::the().current_test_result();
}

// Declared in Macros.h
void set_current_test_result(TestResult result)
{
    TestSuite::the().set_current_test_result(result);
}

// Declared in Macros.h
void set_randomness_source(Randomized::RandomnessSource source)
{
    TestSuite::the().set_randomness_source(move(source));
}

// Declared in Macros.h
Randomized::RandomnessSource& randomness_source()
{
    return TestSuite::the().randomness_source();
}

// Declared in Macros.h
u64 randomized_runs()
{
    return TestSuite::the().randomized_runs();
}

// Declared in TestCase.h
void add_test_case_to_suite(NonnullRefPtr<TestCase> const& test_case)
{
    TestSuite::the().add_case(test_case);
}

// Declared in TestCase.h
void set_suite_setup_function(ByteString const& source, Function<void()> setup)
{
    TestSuite::the().set_suite_setup(source, move(setup));
}

// Declared in Macros.h
bool is_reporting_enabled()
{
    return TestSuite::the().is_reporting_enabled();
}

// Declared in Macros.h
void enable_reporting()
{
    TestSuite::the().enable_reporting();
}

// Declared in Macros.h
void disable_reporting()
{
    TestSuite::the().disable_reporting();
}

static ByteString test_result_to_string(TestResult result)
{
    switch (result) {
    case TestResult::NotRun:
        return "Not run";
    case TestResult::Passed:
        return "Completed";
    case TestResult::Failed:
        return "Failed";
    case TestResult::Rejected:
        return "Rejected";
    case TestResult::Overrun:
        return "Ran out of randomness";
    default:
        return "Unknown TestResult";
    }
}

int TestSuite::main(ByteString const& suite_name, Span<StringView> arguments)
{
    m_suite_name = suite_name;

    Core::ArgsParser args_parser;

    bool do_list_cases = false;
    StringView search_string = "*"sv;
    StringView selected_suite;

    args_parser.add_option(m_randomized_runs, "Number of times to run each RANDOMIZED_TEST_CASE (default 100)", "randomized_runs", 0, "RUNS");
    args_parser.add_option(do_list_cases, "List available test cases.", "list");
    args_parser.add_option(selected_suite, "Only run cases from this suite.", "suite", 0, "SUITE");
    args_parser.add_positional_argument(search_string, "Only run matching cases.", "pattern", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto const& matching_tests = find_cases(search_string, selected_suite);

    if (do_list_cases) {
        outln("Available cases for {}:", suite_name);
        for (auto const& test : matching_tests) {
            outln("    {}::{}", LexicalPath(test->source()).title(), test->name());
        }
        return 0;
    }

    Vector<ByteString> sources;
    for (auto const& test : matching_tests) {
        if (!sources.contains_slow(test->source()))
            sources.append(test->source());
    }
    if (selected_suite.is_empty() && sources.size() > 1) {
        auto executable = Core::System::current_executable_path();
        if (executable.is_error()) {
            warnln("Cannot find the test executable: {}", executable.error());
            return 1;
        }
        int failures = 0;
        for (auto const& source : sources) {
            LexicalPath source_path { source };
            auto suite = source_path.title();
            Vector<ByteString> child_arguments;
            for (auto argument : arguments.slice(1))
                child_arguments.append(ByteString { argument });
            child_arguments.append("--suite");
            child_arguments.append(suite);
            warnln("Running suite '{}'.", suite);
            auto process = Core::Process::spawn(executable.value(), child_arguments);
            if (process.is_error()) {
                warnln("Cannot start the suite: {}", process.error());
                ++failures;
                continue;
            }
            // A suite's process can die without printing anything, so say how it ended. An exit status above 128 is
            // 128 plus the number of the signal that ended it.
            auto status = process.value().wait_for_termination();
            if (status.is_error()) {
                warnln("Cannot wait for suite '{}': {}", suite, status.error());
                ++failures;
            } else if (status.value() != 0) {
                warnln("Suite '{}' exited with status {}.", suite, status.value());
                ++failures;
            }
        }
        return failures;
    }
    if (!selected_suite.is_empty() && matching_tests.is_empty()) {
        warnln("No cases match suite '{}' and pattern '{}'.", selected_suite, search_string);
        return 1;
    }
    if (!sources.is_empty()) {
        if (auto setup = m_setups.find(sources.first()); setup != m_setups.end())
            setup->value();
    }

    outln("Running {} cases out of {}.", matching_tests.size(), m_cases.size());

    return run(matching_tests);
}

Vector<NonnullRefPtr<TestCase>> TestSuite::find_cases(ByteString const& search, StringView suite)
{
    Vector<NonnullRefPtr<TestCase>> matches;
    for (auto& t : m_cases) {
        auto source_path = LexicalPath(t->source());
        if (!suite.is_empty() && source_path.title() != suite)
            continue;
        auto qualified_name = ByteString::formatted("{}::{}", source_path.title(), t->name());
        if (!search.is_empty() && !t->name().matches(search, CaseSensitivity::CaseInsensitive) && !qualified_name.matches(search, CaseSensitivity::CaseInsensitive)) {
            continue;
        }

        matches.append(t);
    }
    return matches;
}

int TestSuite::run(Vector<NonnullRefPtr<TestCase>> const& tests)
{
    size_t test_count = 0;
    size_t test_passed_count = 0;
    size_t test_failed_count = 0;
    TestElapsedTimer global_timer;

    for (auto const& t : tests) {
        warnln("Running test '{}'.", t->name());
        m_current_test_result = TestResult::NotRun;
        enable_reporting();

        TestElapsedTimer timer;
        t->func()();
        auto const test_time = timer.elapsed();

        // Non-randomized tests don't touch the test result when passing.
        if (m_current_test_result == TestResult::NotRun)
            m_current_test_result = TestResult::Passed;

        dbgln("{} test '{}' in {}ms", test_result_to_string(m_current_test_result), t->name(), test_time.to_milliseconds());

        m_test_time += test_time;
        test_count++;

        switch (m_current_test_result) {
        case TestResult::Passed:
            test_passed_count++;
            break;
        case TestResult::Failed:
            test_failed_count++;
            break;
        default:
            break;
        }
    }

    auto const elapsed = global_timer.elapsed();

    dbgln("Finished {} tests in {}ms ({}ms tests, {}ms other).",
        test_count,
        elapsed.to_milliseconds(),
        m_test_time.to_milliseconds(),
        (elapsed - m_test_time).to_milliseconds());

    if (test_count != 0) {
        if (test_passed_count == test_count) {
            dbgln("All {} tests passed.", test_count);
        } else if (test_passed_count + test_failed_count == test_count) {
            dbgln("Out of {} tests, {} passed and {} failed.", test_count, test_passed_count, test_failed_count);
        } else {
            dbgln("Out of {} tests, {} passed, {} failed and {} didn't finish for other reasons.", test_count, test_passed_count, test_failed_count, test_count - test_passed_count - test_failed_count);
        }
    }

    // We have multiple TestResults, all except for Passed being "bad".
    // Let's get a count of them:
    return (int)(test_count - test_passed_count);
}

} // namespace Test
