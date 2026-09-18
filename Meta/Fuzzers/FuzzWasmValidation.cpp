/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Validator.h>
#include <LibWasm/Types.h>

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 65536)
        return 0;
    FixedMemoryStream stream(ReadonlyBytes { data, size });
    auto parsed = Wasm::Module::parse(stream);
    if (parsed.is_error())
        return 0;
    Wasm::AbstractMachine machine;
    // Do not instantiate: untrusted start functions and memory.grow are unbounded.
    // Disable native compilation/cache side effects explicitly, not via environment.
    // Validation still interns immutable types in a process-wide registry. A future
    // runner must bound inputs per process and RSS; destruction does not reset it.
    (void)machine.validate(*parsed.value(), {}, Wasm::CompileToNative::No);
    return 0;
}
