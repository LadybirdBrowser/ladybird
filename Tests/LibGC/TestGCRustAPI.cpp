/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

// NB: Defined in Tests/LibGC/RustAPI, through libgc_api. A failed assertion there panics, which aborts the test.
extern "C" void libgc_api_test_cells_live_while_reachable_and_are_destroyed_once_not();

TEST_CASE(rust_cells_live_while_reachable_and_are_destroyed_once_not)
{
    libgc_api_test_cells_live_while_reachable_and_are_destroyed_once_not();
}
