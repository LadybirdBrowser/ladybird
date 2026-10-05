/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Demangle.h>
#include <AK/StringBuilder.h>

#ifndef AK_OS_WINDOWS
#    include <cxxabi.h>
#    include <stdlib.h>
#    include <string.h>
#endif

static AK::RustSymbolDemangler s_rust_symbol_demangler;

// Every Rust crate registers its demangler on startup, see Libraries/RustDemangle.rs.
void ladybird_set_rust_symbol_demangler(AK::RustSymbolDemangler demangler)
{
    __atomic_store_n(&s_rust_symbol_demangler, demangler, __ATOMIC_RELEASE);
}

namespace AK {

// C++ demanglers leave Rust's own mangling scheme alone, so their output can still contain mangled Rust names.
Optional<ByteString> demangle_rust_symbol(StringView symbol)
{
    // Mach-O symbol tables add an extra leading underscore, while DbgHelp strips it.
    if (!symbol.starts_with("_R"sv) && !symbol.starts_with("__R"sv) && !symbol.starts_with('R'))
        return {};
    auto demangler = __atomic_load_n(&s_rust_symbol_demangler, __ATOMIC_ACQUIRE);
    if (!demangler)
        return {};

    // Mangled names contain neither spaces nor parentheses, but an offset or a parameter list can follow them.
    auto name = symbol.substring_view(0, symbol.find_any_of(" ("sv).value_or(symbol.length()));
    StringBuilder builder;
    auto append = [](void* builder, char const* characters, size_t length) {
        static_cast<StringBuilder*>(builder)->append({ characters, length });
    };
    if (!demangler(name.characters_without_null_termination(), name.length(), append, &builder))
        return {};
    builder.append(symbol.substring_view(name.length()));
    return builder.to_byte_string();
}

// backtrace_symbols() puts the symbol name after a space (macOS) or an opening parenthesis (glibc).
ByteString demangle_backtrace_symbols_line(StringView line)
{
    for (size_t i = 1; i + 1 < line.length(); ++i) {
        if ((line[i - 1] != ' ' && line[i - 1] != '(') || line[i] != '_' || (line[i + 1] != 'Z' && line[i + 1] != 'R'))
            continue;
        auto rest = line.substring_view(i);
        auto length = rest.find_any_of("+ )"sv).value_or(rest.length());
        StringBuilder builder;
        builder.append(line.substring_view(0, i));
        builder.append(demangle(rest.substring_view(0, length)));
        builder.append(rest.substring_view(length));
        return builder.to_byte_string();
    }
    return line;
}

#ifndef AK_OS_WINDOWS
ByteString demangle(StringView name)
{
    if (auto demangled = demangle_rust_symbol(name); demangled.has_value())
        return demangled.release_value();

    int status = 0;
    auto* demangled_name = abi::__cxa_demangle(name.to_byte_string().characters(), nullptr, nullptr, &status);
    auto string = ByteString(status == 0 ? StringView { demangled_name, strlen(demangled_name) } : name);
    if (status == 0)
        free(demangled_name);
    return string;
}
#endif

}
