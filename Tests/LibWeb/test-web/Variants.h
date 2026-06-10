/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "TestWeb.h"

#include <AK/Error.h>

namespace TestWeb {

void apply_variant_to_test(Test& test, String variant);
ErrorOr<void> expand_tests_with_static_variants(Vector<Test>& tests, Vector<ByteString> const& skipped_tests);

}
