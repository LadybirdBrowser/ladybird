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
    Crash,
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
    Vector<FuzzyMatch> fuzzy_matches {};

    RefPtr<Gfx::Bitmap const> actual_screenshot {};
    RefPtr<Gfx::Bitmap const> expectation_screenshot {};

    RefPtr<Core::Timer> timeout_timer {};
};

struct TestCompletion {
    Test& test;
    TestResult result;
};

using TestPromise = Core::Promise<TestCompletion>;

}
