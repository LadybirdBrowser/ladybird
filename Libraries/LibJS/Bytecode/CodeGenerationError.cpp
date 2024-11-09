/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/AST.h>
#include <LibJS/Bytecode/CodeGenerationError.h>

namespace JS::Bytecode {

ErrorOr<String> CodeGenerationError::to_string() const
{
    return String::formatted("CodeGenerationError in {}: {}", failing_node ? failing_node->class_name() : "<unknown node>", reason_literal);
}

}
