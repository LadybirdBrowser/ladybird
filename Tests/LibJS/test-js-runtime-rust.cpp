/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>

extern "C" int libjs_runtime_rust_test_js_main(int argc, char** argv);

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return libjs_runtime_rust_test_js_main(arguments.argc, arguments.argv);
}
