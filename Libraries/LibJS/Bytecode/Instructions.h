/*
 * Copyright (c) 2025, Leon Albrecht <leon.a@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Instruction.h"

#include <AK/Types.h>
#include <LibJS/Bytecode/Op.h>

namespace JS::Bytecode {

template<typename OP>
ALWAYS_INLINE void increment_program_counter(size_t& program_counter, OP const& instruction)
{
    if constexpr (OP::IsVariableLength) {
        program_counter += instruction.length();
    } else {
        program_counter += sizeof(OP);
    }
}

#define INCREMENT_PROGRAM_COUNTER(name)              \
    do {                                             \
        if constexpr (Op::name::IsVariableLength)    \
            program_counter += instruction.length(); \
        else                                         \
            program_counter += sizeof(Op::name);     \
    } while (0)

#if defined(AK_COMPILER_CLANG)
#    define DISPATCH_NEXT()                                                                             \
        do {                                                                                            \
            auto& next_instruction = *reinterpret_cast<Instruction const*>(&bytecode[program_counter]); \
            auto fn = dispatch_instruction_table[static_cast<size_t>(next_instruction.type())];         \
            [[clang::musttail]] return (this->*fn)(bytecode, program_counter);                          \
        } while (0)
#else // GCC does not support musttail, So this can technically cause a stack overflow if the bytecode is to long...
// FIXME: We could hack it to do a tail call by usign inline assembly, but we need to know the calling convention for that.
#    define DISPATCH_NEXT()                                                                             \
        do {                                                                                            \
            auto& next_instruction = *reinterpret_cast<Instruction const*>(&bytecode[program_counter]); \
            auto fn = dispatch_instruction_table[static_cast<size_t>(next_instruction.type())];         \
            return (this->*fn)(bytecode, program_counter);                                              \
        } while (0)
#endif

}
