/*
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Fuzzy.h"

#include <AK/ByteString.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>

namespace TestWeb {

enum class TestMode {
    Layout,
    Text,
    Ref,
    Screenshot,
    Crash,
};

constexpr StringView test_mode_to_string(TestMode mode)
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

enum class TestResult {
    Pass,
    Fail,
    Skipped,
    Timeout,
    Crashed,
    Expanded,
};

constexpr StringView test_result_to_string(TestResult result)
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

enum class RefTestExpectationType {
    Match,
    Mismatch,
};

struct Test {
    TestMode mode;

    ByteString input_path {};
    ByteString expectation_path {};
    ByteString relative_path {};
    ByteString safe_relative_path {};
    Optional<String> variant {};

    UnixDateTime start_time {};
    UnixDateTime end_time {};
    size_t index { 0 };
    size_t run_index { 1 };
    size_t total_runs { 1 };

    String text {};
    bool did_start_test { false };
    bool did_finish_test { false };
    bool did_finish_loading { false };
    bool did_inject_js { false };
    bool did_check_variants { false };

    Optional<RefTestExpectationType> ref_test_expectation_type {};
    Optional<URL::URL> ref_test_expectation_url {};
    Vector<FuzzyMatch> fuzzy_matches {};

    RefPtr<Gfx::Bitmap const> actual_screenshot {};
    RefPtr<Gfx::Bitmap const> expectation_screenshot {};

    u64 diff_pixel_error_count { 0 };
    u8 diff_maximum_error { 0 };

    RefPtr<Core::Timer> timeout_timer {};
};

struct TestCompletion {
    size_t test_index;
    TestResult result;
};

struct ViewDisplayState {
    pid_t pid { 0 };
    ByteString test_name;
    UnixDateTime start_time;
    bool active { false };
};

Vector<ViewDisplayState>& view_states();
size_t total_tests();

using TestPromise = Core::Promise<TestCompletion>;

}
