/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzing/BoundedInput.h"
#include <AK/BitCast.h>
#include <AK/MemoryStream.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/AbstractMachine/Validator.h>

static void unsigned_leb(Vector<u8>& bytes, u32 value)
{
    do {
        auto byte = static_cast<u8>(value & 0x7f);
        value >>= 7;
        bytes.append(byte | (value ? 0x80 : 0));
    } while (value);
}

static void constant(Vector<u8>& code, u32 bits)
{
    code.append(0x41); // i32.const
    auto value = bit_cast<i32>(bits);
    for (;;) {
        auto byte = static_cast<u8>(value & 0x7f);
        value >>= 7;
        bool done = (value == 0 && !(byte & 0x40)) || (value == -1 && (byte & 0x40));
        code.append(byte | (done ? 0 : 0x80));
        if (done)
            break;
    }
}

static void section(Vector<u8>& module, u8 id, Vector<u8> const& content)
{
    module.append(id);
    unsigned_leb(module, content.size());
    module.extend(content);
}

extern "C" int LLVMFuzzerTestOneInput(u8 const* data, size_t size)
{
    AK::set_debug_enabled(false);
    if (size > 4096)
        return 0;
    Fuzzing::BoundedInput input({ data, size });
    Vector<u8> code { 1, 1, 0x7f }; // One local declaration, one i32 local.
    u32 expected = 0;
    constant(code, expected);
    for (size_t step = 0; step < 64 && !input.remaining().is_empty(); ++step) {
        auto operation = input.byte() % 6;
        u32 value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<u32>(input.byte()) << shift;
        switch (operation) {
        case 0:
        case 1:
        case 2:
            constant(code, value);
            code.append(operation == 0 ? 0x6a : operation == 1 ? 0x73
                                                               : 0x6c);
            if (operation == 0)
                expected += value;
            else if (operation == 1)
                expected ^= value;
            else
                expected *= value;
            break;
        case 3:
            constant(code, value | 1);
            code.append(0x6e); // i32.div_u, nonzero divisor.
            expected /= value | 1;
            break;
        case 4:
            // Round-trip the accumulator through a local and bounded linear memory.
            code.extend(Vector<u8> { 0x21, 0, 0x41, 0, 0x20, 0, 0x36, 2, 0, 0x41, 0, 0x28, 2, 0 });
            break;
        case 5:
            // Grow by one page, then discard success/failure without changing the accumulator.
            code.extend(Vector<u8> { 0x41, 1, 0x40, 0, 0x1a });
            break;
        }
    }
    code.append(0x0b);
    Vector<u8> module { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };
    section(module, 1, { 1, 0x60, 0, 1, 0x7f });
    section(module, 3, { 1, 0 });
    section(module, 5, { 1, 1, 1, 2 }); // One memory: minimum 1, maximum 2 pages.
    section(module, 7, { 1, 1, 'f', 0, 0 });
    Vector<u8> body { 1 };
    unsigned_leb(body, code.size());
    body.extend(code);
    section(module, 10, body);

    FixedMemoryStream stream(module.span());
    auto parsed = MUST(Wasm::Module::parse(stream));
    Wasm::AbstractMachine machine;
    machine.enable_instruction_count_limit();
    MUST(machine.validate(*parsed, {}, Wasm::CompileToNative::No));
    auto instance = MUST(machine.instantiate(*parsed, {}));
    VERIFY(instance->exports().size() == 1);
    auto address = instance->exports()[0].value().get<Wasm::FunctionAddress>();
    // Memory persists between calls; stateful growth must not alter arithmetic.
    for (size_t repeat = 0; repeat < 2; ++repeat) {
        auto result = machine.invoke(address, {});
        VERIFY(!result.is_trap());
        VERIFY(result.values().size() == 1);
        VERIFY(result.values()[0].to<u32>() == expected);
    }
    return 0;
}
