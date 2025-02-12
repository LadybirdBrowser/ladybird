/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/Time.h>
#include <LibCore/ArgsParser.h>
#include <LibTest/Macros.h>
#include <LibTest/TestResult.h>
#include <LibTest/TestSuite.h>
#include <math.h>
#include <stdlib.h>
#include <sys/time.h>

namespace Test {

TestSuite* TestSuite::s_global = nullptr;

class TestElapsedTimer {
public:
    TestElapsedTimer() { restart(); }

    void restart() { m_started = UnixDateTime::now(); }

    u64 elapsed_milliseconds()
    {
        return (UnixDateTime::now() - m_started).to_milliseconds();
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
void set_suite_setup_function(Function<void()> setup)
{
    TestSuite::the().set_suite_setup(move(setup));
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

    bool do_tests_only = getenv("TESTS_ONLY") != nullptr;
    bool do_benchmarks_only = false;
    bool do_list_cases = false;
    StringView search_string = "*"sv;

    args_parser.add_option(do_tests_only, "Only run tests.", "tests");
    args_parser.add_option(do_benchmarks_only, "Only run benchmarks.", "bench");
    args_parser.add_option(m_benchmark_repetitions, "Number of times to repeat each benchmark (default 1)", "benchmark_repetitions", 0, "N");
    args_parser.add_option(m_randomized_runs, "Number of times to run each RANDOMIZED_TEST_CASE (default 100)", "randomized_runs", 0, "RUNS");
    args_parser.add_option(do_list_cases, "List available test cases.", "list");
    args_parser.add_positional_argument(search_string, "Only run matching cases.", "pattern", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    if (m_setup)
        m_setup();

    auto const& matching_tests = find_cases(search_string, !do_benchmarks_only, !do_tests_only);

    if (do_list_cases) {
        outln("Available cases for {}:", suite_name);
        for (auto const& test : matching_tests) {
            outln("    {}", test->name());
        }
        return 0;
    }

    outln("Running {} cases out of {}.", matching_tests.size(), m_cases.size());

    return run(matching_tests);
}

Vector<NonnullRefPtr<TestCase>> TestSuite::find_cases(ByteString const& search, bool find_tests, bool find_benchmarks)
{
    Vector<NonnullRefPtr<TestCase>> matches;
    for (auto& t : m_cases) {
        if (!search.is_empty() && !t->name().matches(search, CaseSensitivity::CaseInsensitive)) {
            continue;
        }

        if (!find_tests && !t->is_benchmark()) {
            continue;
        }
        if (!find_benchmarks && t->is_benchmark()) {
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
    size_t benchmark_count = 0;
    size_t benchmark_passed_count = 0;
    size_t benchmark_failed_count = 0;
    TestElapsedTimer global_timer;

    for (auto const& t : tests) {
        auto const test_type = t->is_benchmark() ? "benchmark" : "test";
        auto const repetitions = t->is_benchmark() ? m_benchmark_repetitions : 1;

        warnln("Running {} '{}'.", test_type, t->name());
        m_current_test_result = TestResult::NotRun;
        enable_reporting();

        u64 total_time = 0;
        u64 sum_of_squared_times = 0;
        u64 min_time = NumericLimits<u64>::max();
        u64 max_time = 0;

        for (u64 i = 0; i < repetitions; ++i) {
            TestElapsedTimer timer;
            t->func()();
            auto const iteration_time = timer.elapsed_milliseconds();
            total_time += iteration_time;
            sum_of_squared_times += iteration_time * iteration_time;
            min_time = min(min_time, iteration_time);
            max_time = max(max_time, iteration_time);

            // Non-randomized tests don't touch the test result when passing.
            if (m_current_test_result == TestResult::NotRun)
                m_current_test_result = TestResult::Passed;
        }

        if (repetitions != 1) {
            double average = total_time / double(repetitions);
            double average_squared = average * average;
            double standard_deviation = sqrt((sum_of_squared_times + repetitions * average_squared - 2 * total_time * average) / (repetitions - 1));

            dbgln("{} {} '{}' on average in {:.1f}±{:.1f}ms (min={}ms, max={}ms, total={}ms)",
                test_result_to_string(m_current_test_result), test_type, t->name(),
                average, standard_deviation, min_time, max_time, total_time);
        } else {
            dbgln("{} {} '{}' in {}ms", test_result_to_string(m_current_test_result), test_type, t->name(), total_time);
        }

        if (t->is_benchmark()) {
            m_benchtime += total_time;
            benchmark_count++;

            switch (m_current_test_result) {
            case TestResult::Passed:
                benchmark_passed_count++;
                break;
            case TestResult::Failed:
                benchmark_failed_count++;
                break;
            default:
                break;
            }
        } else {
            m_testtime += total_time;
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
    }

    dbgln("Finished {} tests and {} benchmarks in {}ms ({}ms tests, {}ms benchmarks, {}ms other).",
        test_count,
        benchmark_count,
        global_timer.elapsed_milliseconds(),
        m_testtime,
        m_benchtime,
        global_timer.elapsed_milliseconds() - (m_testtime + m_benchtime));

    if (test_count != 0) {
        if (test_passed_count == test_count) {
            dbgln("All {} tests passed.", test_count);
        } else if (test_passed_count + test_failed_count == test_count) {
            dbgln("Out of {} tests, {} passed and {} failed.", test_count, test_passed_count, test_failed_count);
        } else {
            dbgln("Out of {} tests, {} passed, {} failed and {} didn't finish for other reasons.", test_count, test_passed_count, test_failed_count, test_count - test_passed_count - test_failed_count);
        }
    }

    if (benchmark_count != 0) {
        if (benchmark_passed_count == benchmark_count) {
            dbgln("All {} benchmarks passed.", benchmark_count);
        } else if (benchmark_passed_count + benchmark_failed_count == benchmark_count) {
            dbgln("Out of {} benchmarks, {} passed and {} failed.", benchmark_count, benchmark_passed_count, benchmark_failed_count);
        } else {
            dbgln("Out of {} benchmarks, {} passed, {} failed and {} didn't finish for other reasons.", benchmark_count, benchmark_passed_count, benchmark_failed_count, benchmark_count - benchmark_passed_count - benchmark_failed_count);
        }
    }

    // We have multiple TestResults, all except for Passed being "bad".
    // Let's get a count of them:
    return (int)(test_count - test_passed_count + benchmark_count - benchmark_passed_count);
}

} // namespace Test
