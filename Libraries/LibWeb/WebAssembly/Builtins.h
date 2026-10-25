/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWasm/Runtime.h>

namespace Web::WebAssembly::Detail {

bool validate_builtins_and_imported_strings(Wasm::Runtime::Module const&, ReadonlySpan<Utf16String> builtin_set_names, Optional<Utf16String> const& imported_string_module);

// Whether finding a builtin for the import and builtinSetNames does not return null.
bool import_refers_to_a_builtin(Wasm::Runtime::Import const&, ReadonlySpan<Utf16String> builtin_set_names);

bool refers_to_a_builtin_set(Utf16String const& builtin_set_name);
GC::Ref<JS::Object> instantiate_a_builtin_set(JS::VM&, Utf16String const& builtin_set_name);

GC::Ref<JS::Object> instantiate_imported_strings(JS::VM&, Wasm::Runtime::Module const&, Utf16String const& imported_string_module);

}
