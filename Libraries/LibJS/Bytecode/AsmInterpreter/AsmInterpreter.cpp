/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/AsmInterpreter/AsmInterpreter.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

extern "C" void asm_register_slow_path_stats();
namespace JS::Bytecode {

// Defined in generated assembly (asmint_x86_64.S or asmint_aarch64.S)
extern "C" void asm_interpreter_entry(u8 const* bytecode, u32 entry_point, Value* values, VM* vm);

void AsmInterpreter::run(VM& vm, size_t entry_point)
{
    asm_register_slow_path_stats();

    auto& context = vm.running_execution_context();
    auto* bytecode = context.executable->bytecode.data();
    auto* values = context.registers_and_constants_and_locals_and_arguments_span().data();

    asm_interpreter_entry(bytecode, static_cast<u32>(entry_point), values, &vm);
}

}
