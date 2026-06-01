/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWasm/Types.h>

namespace Wasm {

bool try_cranelift_compile(CompiledInstructions&, u32) { return false; }
void flush_cranelift_batch() { }
void discard_cranelift_batch() { }
void free_cranelift_code(void*) { }
void set_cranelift_active_function_index(u32) { }
void begin_cranelift_cache_capture() { }
void abort_cranelift_cache_capture() { }
void abort_cranelift_cache_install() { }
Optional<ByteBuffer> serialize_cranelift_cache_blob(ReadonlyBytes) { return {}; }
bool try_install_cranelift_cache_blob(ReadonlyBytes, ReadonlyBytes) { return false; }

}
