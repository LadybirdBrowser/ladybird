/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibXML/Export.h>
#include <LibXML/RustFFI.h>

namespace XML::XPath {

// The XPath compiler is part of the Rust crate of this library, and a library exports only what it declares itself.
// These are the way to it for the library that evaluates the programs it makes.

// Compiles an XPath 1.0 expression. Returns null if it isn't one.
XML_API FFI::XPathProgram* compile(ReadonlyBytes source);

// Pointers into a compiled expression, good until it is freed.
XML_API FFI::XPathProgramView view(FFI::XPathProgram const&);

XML_API void free(FFI::XPathProgram*);

}
