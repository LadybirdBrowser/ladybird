/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <LibTest/Macros.h>
#include <LibTest/Randomized/RandomnessSource.h>
#include <LibTest/Randomized/Shrink.h>

namespace Test {

using TestFunction = Function<void()>;

inline void run_with_randomness_source(Randomized::RandomnessSource source, TestFunction const& test_function)
{
    set_randomness_source(move(source));
    set_current_test_result(TestResult::NotRun);
    test_function();
    if (current_test_result() == TestResult::NotRun) {
        set_current_test_result(TestResult::Passed);
    }
}

class TestCase : public RefCounted<TestCase> {
public:
    TestCase(ByteString const& name, TestFunction&& fn, bool is_benchmark)
        : m_name(name)
        , m_function(move(fn))
        , m_is_benchmark(is_benchmark)
    {
    }

    bool is_benchmark() const { return m_is_benchmark; }
    ByteString const& name() const { return m_name; }
    TestFunction const& func() const { return m_function; }

    static NonnullRefPtr<TestCase> randomized(ByteString const& name, TestFunction&& test_function)
    {
        using namespace Randomized;

        constexpr u8 MAX_GEN_ATTEMPTS_PER_VALUE = 30;

        TestFunction test_case_function = [test_function = move(test_function)]() {
            u64 max_randomized_runs = randomized_runs();
            for (u64 i = 0; i < max_randomized_runs; ++i) {
                bool generated_successfully = false;
                u8 gen_attempt;
                for (gen_attempt = 0; gen_attempt < MAX_GEN_ATTEMPTS_PER_VALUE && !generated_successfully; ++gen_attempt) {
                    // We're going to run the test function many times, so let's turn off the reporting until we finish.
                    disable_reporting();

                    set_current_test_result(TestResult::NotRun);
                    run_with_randomness_source(RandomnessSource::live(), test_function);
                    switch (current_test_result()) {
                    case TestResult::NotRun:
                        VERIFY_NOT_REACHED();
                        break;
                    case TestResult::Passed: {
                        generated_successfully = true;
                        break;
                    }
                    case TestResult::Failed: {
                        generated_successfully = true;
                        RandomRun first_failure = randomness_source().run();
                        RandomRun best_failure = shrink(first_failure, test_function);

                        // Run one last time with reporting on, so that the user can see the minimal failure
                        enable_reporting();
                        run_with_randomness_source(RandomnessSource::recorded(best_failure), test_function);
                        return;
                    }
                    case TestResult::Rejected:
                        break;
                    case TestResult::Overrun:
                        break;
                    default:
                        VERIFY_NOT_REACHED();
                        break;
                    }
                }
                enable_reporting();
                if (!generated_successfully) {
                    // The loop above got to the full MAX_GEN_ATTEMPTS_PER_VALUE and gave up.
                    // Run one last time with reporting on, so that the user gets the REJECTED message.
                    RandomRun last_failure = randomness_source().run();
                    run_with_randomness_source(RandomnessSource::recorded(last_failure), test_function);
                    return;
                }
            }
            // All randomized_runs() values generated + passed the test.
        };
        return make_ref_counted<TestCase>(name, move(test_case_function), false);
    }

private:
    ByteString m_name;
    TestFunction m_function;
    bool m_is_benchmark;
};

// Helper to hide implementation of TestSuite from users
void add_test_case_to_suite(NonnullRefPtr<TestCase> const& test_case);
void set_suite_setup_function(Function<void()> setup);
}

#define TEST_SETUP                                   \
    static void __setup();                           \
    struct __setup_type {                            \
        __setup_type()                               \
        {                                            \
            Test::set_suite_setup_function(__setup); \
        }                                            \
    };                                               \
    static struct __setup_type __setup_type;         \
    static void __setup()

// Unit test

#define __TESTCASE_FUNC(x) __test_##x
#define __TESTCASE_TYPE(x) __TestCase_##x

#define TEST_CASE(x)                                                                                 \
    static void __TESTCASE_FUNC(x)();                                                                \
    struct __TESTCASE_TYPE(x) {                                                                      \
        __TESTCASE_TYPE(x)                                                                           \
        ()                                                                                           \
        {                                                                                            \
            add_test_case_to_suite(adopt_ref(*new ::Test::TestCase(#x, __TESTCASE_FUNC(x), false))); \
        }                                                                                            \
    };                                                                                               \
    static struct __TESTCASE_TYPE(x) __TESTCASE_TYPE(x);                                             \
    static void __TESTCASE_FUNC(x)()

// Benchmark

#define __BENCHMARK_FUNC(x) __benchmark_##x
#define __BENCHMARK_TYPE(x) __BenchmarkCase_##x

#define BENCHMARK_CASE(x)                                                                            \
    static void __BENCHMARK_FUNC(x)();                                                               \
    struct __BENCHMARK_TYPE(x) {                                                                     \
        __BENCHMARK_TYPE(x)                                                                          \
        ()                                                                                           \
        {                                                                                            \
            add_test_case_to_suite(adopt_ref(*new ::Test::TestCase(#x, __BENCHMARK_FUNC(x), true))); \
        }                                                                                            \
    };                                                                                               \
    static struct __BENCHMARK_TYPE(x) __BENCHMARK_TYPE(x);                                           \
    static void __BENCHMARK_FUNC(x)()

// Randomized test

#define __RANDOMIZED_TEST_FUNC(x) __randomized_test_##x
#define __RANDOMIZED_TEST_TYPE(x) __RandomizedTestCase_##x

#define RANDOMIZED_TEST_CASE(x)                                                                  \
    static void __RANDOMIZED_TEST_FUNC(x)();                                                     \
    struct __RANDOMIZED_TEST_TYPE(x) {                                                           \
        __RANDOMIZED_TEST_TYPE(x)                                                                \
        ()                                                                                       \
        {                                                                                        \
            add_test_case_to_suite(::Test::TestCase::randomized(#x, __RANDOMIZED_TEST_FUNC(x))); \
        }                                                                                        \
    };                                                                                           \
    static struct __RANDOMIZED_TEST_TYPE(x) __RANDOMIZED_TEST_TYPE(x);                           \
    static void __RANDOMIZED_TEST_FUNC(x)()

// This allows us to print the generated locals in the test after a failure is fully shrunk.
#define GEN(identifier, value)                                        \
    auto identifier = (value);                                        \
    if (::Test::current_test_result() == ::Test::TestResult::Overrun) \
        return;                                                       \
    if (::Test::is_reporting_enabled())                               \
    ::AK::warnln("{} = {}", #identifier, (identifier))
