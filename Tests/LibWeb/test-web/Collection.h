/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

namespace TestWeb {

class Application;

bool is_valid_test_name(StringView test_name);

ErrorOr<void> collect_crash_tests(Application const&, Vector<Test>& tests, StringView path, StringView trail);
ErrorOr<void> collect_dump_tests(Application const&, Vector<Test>& tests, StringView path, StringView trail, TestMode);
ErrorOr<void> collect_ref_tests(Application const&, Vector<Test>& tests, StringView path, StringView trail);
ErrorOr<void> collect_screenshot_tests(Application const&, Vector<Test>& tests, StringView path, StringView trail);

}
