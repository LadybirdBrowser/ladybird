/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibTest/Macros.h>
#include <LibTest/Randomized/RandomnessSource.h>
#include <LibTest/TestCase.h>
#include <LibTest/TestResult.h>

namespace Test {

class TestSuite {
public:
    static TestSuite& the()
    {
        if (s_global == nullptr)
            s_global = new TestSuite();
        return *s_global;
    }

    static void release()
    {
        if (s_global)
            delete s_global;
        s_global = nullptr;
    }

    int run(Vector<NonnullRefPtr<TestCase>> const&);
    int main(ByteString const& suite_name, Span<StringView> arguments);
    Vector<NonnullRefPtr<TestCase>> find_cases(ByteString const& search, bool find_tests, bool find_benchmarks);
    void add_case(NonnullRefPtr<TestCase> const& test_case)
    {
        m_cases.append(test_case);
    }

    TestResult current_test_result() const { return m_current_test_result; }
    void set_current_test_result(TestResult result) { m_current_test_result = result; }

    void set_suite_setup(Function<void()> setup) { m_setup = move(setup); }
    // The RandomnessSource is where generators record / replay random data
    // from. Initially a live "truly random" RandomnessSource is used, and when
    // a failure is found, a set of hardcoded RandomnessSources is used during
    // shrinking.
    void set_randomness_source(Randomized::RandomnessSource source) { m_randomness_source = move(source); }
    Randomized::RandomnessSource& randomness_source() { return m_randomness_source; }

    // Dictates whether FAIL(), EXPECT() and similar macros in LibTest/Macros.h
    // print messages or not. This is important for randomized tests because
    // they run the test function many times in a row, and we only want to
    // report the _minimal_ (shrunk) failure to the user, not all of them.
    bool is_reporting_enabled() { return m_reporting_enabled; }
    void enable_reporting() { m_reporting_enabled = true; }
    void disable_reporting() { m_reporting_enabled = false; }

    u64 randomized_runs() { return m_randomized_runs; }

private:
    static TestSuite* s_global;
    Vector<NonnullRefPtr<TestCase>> m_cases;
    u64 m_testtime = 0;
    u64 m_benchtime = 0;
    ByteString m_suite_name;
    u64 m_benchmark_repetitions = 1;
    u64 m_randomized_runs = 100;
    Function<void()> m_setup;
    TestResult m_current_test_result = TestResult::NotRun;
    Randomized::RandomnessSource m_randomness_source = Randomized::RandomnessSource::live();
    bool m_reporting_enabled = true;
};

}
