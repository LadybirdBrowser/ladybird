/*
 * Copyright (c) 2026-present, Ali Mohammad Pur <ali@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWasm/Types.h>

namespace Wasm {

bool try_cranelift_compile(CompiledInstructions&, u32) { return false; }
void flush_cranelift_batch() { }
void free_cranelift_code(void*) { }

}
