/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWasm/Runtime.h>
#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    AK::set_debug_enabled(false);
    // Compiling a module decodes and validates it.
    [[maybe_unused]] auto result = Wasm::Runtime::Module::compile(ReadonlyBytes { data, size });
    return 0;
}
