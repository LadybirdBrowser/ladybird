/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace JS {

bool compare_pipelines_enabled();

void compare_pipeline_asts(StringView rust_ast, StringView cpp_ast, StringView context);
void compare_pipeline_bytecode(StringView rust_bytecode, StringView cpp_bytecode, StringView context, StringView ast_dump = {});

}
