/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <LibWGSL/Export.h>

namespace WGSL {

class WGSL_API Preprocessor {

public:
    explicit Preprocessor(StringView input);

    ErrorOr<String> process();

private:
    GenericLexer m_lexer;

    ErrorOr<String> compute_remove_comments();
};

}
