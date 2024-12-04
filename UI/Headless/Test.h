/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibCore/Promise.h>
#include <LibGfx/Forward.h>
#include <LibURL/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Ladybird {

class HeadlessWebView;

enum class TestMode {
    Layout,
    Text,
    Ref,
};

enum class TestResult {
    Pass,
    Fail,
    Skipped,
    Timeout,
    Crashed,
};

enum class RefTestExpectationType {
    Match,
    Mismatch,
};

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
    }
    VERIFY_NOT_REACHED();
}

struct Test {
    TestMode mode;

    ByteString input_path {};
    ByteString expectation_path {};
    ByteString relative_path {};

    UnixDateTime start_time {};
    UnixDateTime end_time {};
    size_t index { 0 };

    String text {};
    bool did_finish_test { false };
    bool did_finish_loading { false };

    Optional<RefTestExpectationType> ref_test_expectation_type {};

    RefPtr<Gfx::Bitmap> actual_screenshot {};
    RefPtr<Gfx::Bitmap> expectation_screenshot {};
};

struct TestCompletion {
    Test& test;
    TestResult result;
};

using TestPromise = Core::Promise<TestCompletion>;

ErrorOr<void> run_tests(Core::AnonymousBuffer const& theme, Web::DevicePixelSize window_size);
void run_dump_test(HeadlessWebView&, Test&, URL::URL const&, int timeout_in_milliseconds);

}
