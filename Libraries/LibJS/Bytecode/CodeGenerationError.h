/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Forward.h>

namespace JS::Bytecode {

struct CodeGenerationError {
    ASTNode const* failing_node { nullptr };
    StringView reason_literal;

    ErrorOr<String> to_string() const;
};

template<typename T>
using CodeGenerationErrorOr = ErrorOr<T, CodeGenerationError>;

}
