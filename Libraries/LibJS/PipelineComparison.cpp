/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/PipelineComparison.h>
#include <stdlib.h>

namespace JS {

bool compare_pipelines_enabled()
{
    static bool const enabled = getenv("LIBJS_COMPARE_PIPELINES") != nullptr;
    return enabled;
}

static void report_mismatch(StringView kind, StringView rust_dump, StringView cpp_dump, StringView context)
{
    StringBuilder message;
    message.appendff("PIPELINE MISMATCH ({}) in: {}\n", kind, context);
    message.appendff("\n=== Rust {} ===\n{}\n", kind, rust_dump);
    message.appendff("\n=== C++ {} ===\n{}\n", kind, cpp_dump);
    warnln("{}", message.string_view());
    VERIFY_NOT_REACHED();
}

void compare_pipeline_asts(StringView rust_ast, StringView cpp_ast, StringView context)
{
    if (rust_ast != cpp_ast)
        report_mismatch("AST"sv, rust_ast, cpp_ast, context);
}

void compare_pipeline_bytecode(StringView rust_bytecode, StringView cpp_bytecode, StringView context, StringView ast_dump)
{
    if (rust_bytecode != cpp_bytecode) {
        if (!ast_dump.is_empty())
            warnln("\n=== AST (both identical) ===\n{}", ast_dump);
        report_mismatch("Bytecode"sv, rust_bytecode, cpp_bytecode, context);
    }
}

}
