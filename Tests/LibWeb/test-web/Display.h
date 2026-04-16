/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/ByteString.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibDiff/Format.h>

namespace TestWeb {

struct Display {
    static Display& the();

    void begin_run();
    void on_test_started(size_t view_index, Test const&, pid_t);
    void on_test_finished(size_t view_index, Test const&, TestResult);
    void on_fail_fast(Test const&, TestResult, pid_t);
    void print_run_complete(ReadonlySpan<Test>, ReadonlySpan<TestCompletion>, size_t tests_remaining) const;
    void print_failure_diff(URL::URL const&, Test const&, ByteBuffer const& expectation) const;
    void render_live_display() const;
    void clear_live_display();

    RefPtr<Core::Timer> display_timer;
    Vector<ByteString> deferred_warnings;
    size_t completed_tests { 0 };
    size_t pass_count { 0 };
    size_t fail_count { 0 };
    size_t timeout_count { 0 };
    size_t crashed_count { 0 };
    size_t skipped_count { 0 };
    size_t current_run { 1 };
    bool is_tty { false };
    bool is_live_display_active { false };
};

}
