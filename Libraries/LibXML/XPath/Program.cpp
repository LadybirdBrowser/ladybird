/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXML/XPath/Program.h>

namespace XML::XPath {

FFI::XPathProgram* compile(ReadonlyBytes source)
{
    return FFI::rust_xpath_compile(source.data(), source.size());
}

FFI::XPathProgramView view(FFI::XPathProgram const& program)
{
    return FFI::rust_xpath_program_view(&program);
}

void free(FFI::XPathProgram* program)
{
    FFI::rust_xpath_program_free(program);
}

}
