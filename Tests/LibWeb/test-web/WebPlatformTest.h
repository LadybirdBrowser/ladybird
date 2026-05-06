/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>

namespace TestWeb {

class Fixture;

NonnullOwnPtr<Fixture> create_web_platform_test_fixture();
ErrorOr<void> collect_wpt_tests(Vector<Test>&);
ErrorOr<TestResult> on_wpt_test_result(Test&, URL::URL const&);
URL::URL wpt_url(StringView relative_path);

} // namespace TestWeb
