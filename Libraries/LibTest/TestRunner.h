/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibTest/Results.h>
#include <LibTest/TestRunnerUtil.h>

namespace Test {

class TestRunner {
public:
    static TestRunner* the()
    {
        return s_the;
    }

    TestRunner(ByteString test_root, bool print_times, bool print_progress, bool print_json, bool detailed_json = false)
        : m_test_root(move(test_root))
        , m_print_times(print_times)
        , m_print_progress(print_progress)
        , m_print_json(print_json)
        , m_detailed_json(detailed_json)
    {
        VERIFY(!s_the);
        s_the = this;
    }

    virtual ~TestRunner() { s_the = nullptr; }

    virtual void run(ReadonlySpan<ByteString> test_globs);

    Test::Counts const& counts() const { return m_counts; }

    bool is_printing_progress() const { return m_print_progress; }

    bool needs_detailed_suites() const { return m_detailed_json; }
    Vector<Test::Suite> const& suites() const { return *m_suites; }

    Vector<Test::Suite>& ensure_suites()
    {
        if (!m_suites.has_value())
            m_suites = Vector<Suite> {};
        return *m_suites;
    }

protected:
    static TestRunner* s_the;

    void print_test_results() const;
    void print_test_results_as_json() const;

    virtual Vector<ByteString> get_test_paths() const = 0;
    virtual void do_run_single_test(ByteString const&, size_t current_test_index, size_t num_tests) = 0;
    virtual Vector<ByteString> const* get_failed_test_names() const { return nullptr; }

    ByteString m_test_root;
    bool m_print_times;
    bool m_print_progress;
    bool m_print_json;
    bool m_detailed_json;

    double m_total_elapsed_time_in_ms { 0 };
    Test::Counts m_counts;
    Optional<Vector<Test::Suite>> m_suites;
};

inline void cleanup()
{
    // Clear the taskbar progress.
    if (TestRunner::the() && TestRunner::the()->is_printing_progress())
        warn("\033]9;-1;\033\\");
}

[[noreturn]] inline void cleanup_and_exit()
{
    cleanup();
    exit(1);
}

inline void TestRunner::run(ReadonlySpan<ByteString> test_globs)
{
    size_t progress_counter = 0;
    auto test_paths = get_test_paths();
    for (auto& path : test_paths) {
        if (!any_of(test_globs, [&](auto& glob) { return path.matches(glob); }))
            continue;
        ++progress_counter;
        do_run_single_test(path, progress_counter, test_paths.size());
        if (m_print_progress)
            warn("\033]9;{};{};\033\\", progress_counter, test_paths.size());
    }

    if (m_print_progress)
        warn("\033]9;-1;\033\\");

    if (!m_print_json)
        print_test_results();
    else
        print_test_results_as_json();
}

enum Modifier {
    BG_RED,
    BG_GREEN,
    FG_RED,
    FG_GREEN,
    FG_ORANGE,
    FG_GRAY,
    FG_BLACK,
    FG_BOLD,
    ITALIC,
    CLEAR,
};

inline void print_modifiers(Vector<Modifier> modifiers)
{
    for (auto& modifier : modifiers) {
        auto code = [&] {
            switch (modifier) {
            case BG_RED:
                return "\033[41m";
            case BG_GREEN:
                return "\033[42m";
            case FG_RED:
                return "\033[31m";
            case FG_GREEN:
                return "\033[32m";
            case FG_ORANGE:
                return "\033[33m";
            case FG_GRAY:
                return "\033[90m";
            case FG_BLACK:
                return "\033[30m";
            case FG_BOLD:
                return "\033[1m";
            case ITALIC:
                return "\033[3m";
            case CLEAR:
                return "\033[0m";
            }
            VERIFY_NOT_REACHED();
        }();
        out("{}", code);
    }
}

inline void TestRunner::print_test_results() const
{
    out("\nTest Suites: ");
    if (m_counts.suites_failed) {
        print_modifiers({ FG_RED });
        out("{} failed, ", m_counts.suites_failed);
        print_modifiers({ CLEAR });
    }
    if (m_counts.suites_passed) {
        print_modifiers({ FG_GREEN });
        out("{} passed, ", m_counts.suites_passed);
        print_modifiers({ CLEAR });
    }
    outln("{} total", m_counts.suites_failed + m_counts.suites_passed);

    out("Tests:       ");
    if (m_counts.tests_failed) {
        print_modifiers({ FG_RED });
        out("{} failed, ", m_counts.tests_failed);
        print_modifiers({ CLEAR });
    }
    if (m_counts.tests_skipped) {
        print_modifiers({ FG_ORANGE });
        out("{} skipped, ", m_counts.tests_skipped);
        print_modifiers({ CLEAR });
    }
    if (m_counts.tests_expected_failed) {
        print_modifiers({ FG_ORANGE });
        out("{} expected failed, ", m_counts.tests_expected_failed);
        print_modifiers({ CLEAR });
    }
    if (m_counts.tests_passed) {
        print_modifiers({ FG_GREEN });
        out("{} passed, ", m_counts.tests_passed);
        print_modifiers({ CLEAR });
    }
    outln("{} total", m_counts.tests_failed + m_counts.tests_skipped + m_counts.tests_passed + m_counts.tests_expected_failed);

    outln("Files:       {} total", m_counts.files_total);

    out("Time:        ");
    if (m_total_elapsed_time_in_ms < 1000.0) {
        outln("{}ms", static_cast<int>(m_total_elapsed_time_in_ms));
    } else {
        outln("{:>.3}s", m_total_elapsed_time_in_ms / 1000.0);
    }
    if (auto* failed_tests = get_failed_test_names(); failed_tests && !failed_tests->is_empty()) {
        outln("Failed tests: {}", *failed_tests);
    }
    outln();
}

inline void TestRunner::print_test_results_as_json() const
{
    JsonObject root;
    if (needs_detailed_suites()) {
        auto& suites = this->suites();
        u64 duration_us = 0;
        JsonObject tests;

        for (auto& suite : suites) {
            for (auto& case_ : suite.tests) {
                duration_us += case_.duration_us;
                StringView result_name;
                switch (case_.result) {
                case Result::Pass:
                    result_name = "PASSED"sv;
                    break;
                case Result::Fail:
                    result_name = "FAILED"sv;
                    break;
                case Result::Skip:
                    result_name = "SKIPPED"sv;
                    break;
                case Result::ExpectedFail:
                    result_name = "XFAIL"sv;
                    break;
                case Result::Crashed:
                    result_name = "PROCESS_ERROR"sv;
                    break;
                }

                auto name = suite.name;
                if (name == "__$$TOP_LEVEL$$__"sv)
                    name = ByteString::empty();

                auto path = *LexicalPath::relative_path(suite.path, m_test_root);

                tests.set(ByteString::formatted("{}/{}::{}", path, name, case_.name), result_name);
            }
        }

        root.set("duration", static_cast<double>(duration_us) / 1000000.);
        root.set("results", move(tests));
    } else {
        JsonObject suites;
        suites.set("failed", m_counts.suites_failed);
        suites.set("passed", m_counts.suites_passed);
        suites.set("total", m_counts.suites_failed + m_counts.suites_passed);

        JsonObject tests;
        tests.set("failed", m_counts.tests_failed);
        tests.set("passed", m_counts.tests_passed);
        tests.set("skipped", m_counts.tests_skipped);
        tests.set("xfail", m_counts.tests_expected_failed);
        tests.set("total", m_counts.tests_failed + m_counts.tests_passed + m_counts.tests_skipped + m_counts.tests_expected_failed);

        JsonObject results;
        results.set("suites", suites);
        results.set("tests", tests);

        root.set("results", results);
        root.set("files_total", m_counts.files_total);
        root.set("duration", m_total_elapsed_time_in_ms / 1000.0);
    }
    outln("{}", root.to_byte_string());
}

}
