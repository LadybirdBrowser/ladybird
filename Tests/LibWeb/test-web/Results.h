/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/ByteBuffer.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <LibGfx/Forward.h>

namespace TestWeb {

void append_result(Test const&, TestResult);
ErrorOr<ByteString> prepare_output_path(Test const& test);
ErrorOr<void> setup_results_directory();
ErrorOr<void> dump_screenshot_to_file(Gfx::Bitmap const&, StringView path);
ErrorOr<void> prepare_result_files(ReadonlySpan<Test>);
ErrorOr<void> write_test_diff_to_results(Test const&, ByteBuffer const& expectation);
ErrorOr<void> write_screenshot_failure_results(Test&, Gfx::Bitmap const& actual, Gfx::Bitmap const& expected);

}
