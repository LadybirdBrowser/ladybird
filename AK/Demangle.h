/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>

namespace AK {

ByteString demangle(StringView name);
Optional<ByteString> demangle_rust_symbol(StringView symbol);
ByteString demangle_backtrace_symbols_line(StringView line);

using RustSymbolDemangler = bool (*)(char const* name, size_t name_length, void (*append)(void* context, char const*, size_t), void* context);

}

extern "C" void ladybird_set_rust_symbol_demangler(AK::RustSymbolDemangler);

#if USING_AK_GLOBALLY
using AK::demangle;
using AK::demangle_backtrace_symbols_line;
using AK::demangle_rust_symbol;
#endif
