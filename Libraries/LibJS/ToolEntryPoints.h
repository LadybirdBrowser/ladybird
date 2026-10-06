/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibJS/Export.h>

// The line editor that the js tool hands to the runtime's REPL. The tool defines it, so that only the tool links
// libedit.
struct JSLineEditor;

// The js, test262-runner and test-js-runtime tools are written in the runtime. These are how their C++ mains reach
// them, as LibJS hides the runtime's own symbols.
namespace JS {

JS_API int js_main(int argc, char** argv, JSLineEditor const* line_editor);
#if !defined(AK_OS_WINDOWS)
JS_API int test262_runner_main(int argc, char** argv);
#endif
JS_API int test_js_runtime_main(int argc, char** argv);

}
