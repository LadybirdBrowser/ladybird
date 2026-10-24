/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>

// What LibWeb's C++ code asks of the WebAssembly JavaScript Interface, which is implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/web_assembly).
namespace Web::WebAssembly {

// A WebAssembly.CompileError in the realm.
GC::Ref<JS::Object> create_compile_error(JS::Realm&, Utf16String const& message);

// https://webassembly.github.io/spec/js-api/#HostResizeArrayBuffer
JS::ThrowCompletionOr<JS::HandledByHost> host_resize_array_buffer(JS::VM&, JS::ArrayBuffer&, size_t new_length);

// https://webassembly.github.io/threads/js-api/#abstract-operation-hostgrowsharedarraybuffer
JS::ThrowCompletionOr<JS::HandledByHost> host_grow_shared_array_buffer(JS::VM&, JS::ArrayBuffer&, size_t new_length);

// https://webassembly.github.io/esm-integration/js-api/index.html#parse-a-webassembly-module
// The result is a WebAssembly Module Record: a host module whose abstract methods are those of WebAssembly modules.
JS::ThrowCompletionOr<GC::Ref<JS::HostModule>> parse_a_webassembly_module(ReadonlyBytes, JS::Realm&, StringView filename, GC::Ptr<GC::Cell> host_defined);

}
