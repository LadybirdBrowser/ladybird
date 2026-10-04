/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMain/Main.h>

#ifdef LIBJS_TOOL_ENTRY_POINTS_IN_FACADE
#    include <LibJS/ToolEntryPoints.h>
#else
extern "C" int libjs_runtime_rust_test262_runner_main(int argc, char** argv);

namespace JS {

static int test262_runner_main(int argc, char** argv)
{
    return libjs_runtime_rust_test262_runner_main(argc, argv);
}

}
#endif

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    return JS::test262_runner_main(arguments.argc, arguments.argv);
}
