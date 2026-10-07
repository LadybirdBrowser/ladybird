/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/ToolEntryPoints.h>

extern "C" {
int libjs_rust_js_main(int argc, char** argv, JSLineEditor const* line_editor);
#if !defined(AK_OS_WINDOWS)
int libjs_rust_test262_runner_main(int argc, char** argv);
#endif
int libjs_rust_test_js_main(int argc, char** argv);
}

namespace JS {

int js_main(int argc, char** argv, JSLineEditor const* line_editor)
{
    return libjs_rust_js_main(argc, argv, line_editor);
}

#if !defined(AK_OS_WINDOWS)
int test262_runner_main(int argc, char** argv)
{
    return libjs_rust_test262_runner_main(argc, argv);
}
#endif

int test_js_runtime_main(int argc, char** argv)
{
    return libjs_rust_test_js_main(argc, argv);
}

}
