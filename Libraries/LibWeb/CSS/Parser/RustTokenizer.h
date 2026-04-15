/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/Token.h>
#include <LibWeb/Export.h>

namespace Web::CSS::Parser::FFI {

struct CssToken;

}

namespace Web::CSS::Parser {

class WEB_API RustTokenizer {
public:
    static Vector<Token> tokenize(StringView input, StringView encoding);

private:
    static Token token_from_ffi(FFI::CssToken const&);
};

}
