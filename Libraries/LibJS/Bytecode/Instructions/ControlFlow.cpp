/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../Instructions.h"
#include "AK/Platform.h"
#include "LibJS/Bytecode/Op.h"

#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/GeneratorResult.h>

namespace JS::Bytecode {

ALWAYS_INLINE Value Interpreter::do_yield(Value value, Optional<Label> continuation)
{
    // FIXME: If we get a pointer, which is not accurately representable as a double
    //        will cause this to explode
    auto continuation_value = continuation.has_value() ? Value(continuation->address()) : js_null();
    return vm().heap().allocate<GeneratorResult>(value, continuation_value, false).ptr();
}

FLATTEN void Interpreter::handle_Mov(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::Mov const*>(&bytecode[program_counter]);
    set(instruction.dst(), get(instruction.src()));
    INCREMENT_PROGRAM_COUNTER(Mov);
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_End(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::End const*>(&bytecode[program_counter]);
    accumulator() = get(instruction.value());
}

FLATTEN void Interpreter::handle_Jump(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::Jump const*>(&bytecode[program_counter]);
    program_counter = instruction.target().address();
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_JumpIf(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::JumpIf const*>(&bytecode[program_counter]);
    if (get(instruction.condition()).to_boolean())
        program_counter = instruction.true_target().address();
    else
        program_counter = instruction.false_target().address();

    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_JumpTrue(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::JumpTrue const*>(&bytecode[program_counter]);
    if (get(instruction.condition()).to_boolean())
        program_counter = instruction.target().address();
    else
        INCREMENT_PROGRAM_COUNTER(JumpTrue);

    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_JumpFalse(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::JumpFalse const*>(&bytecode[program_counter]);
    if (!get(instruction.condition()).to_boolean())
        program_counter = instruction.target().address();
    else
        INCREMENT_PROGRAM_COUNTER(JumpFalse);
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_JumpNullish(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::JumpNullish const*>(&bytecode[program_counter]);
    if (get(instruction.condition()).is_nullish())
        program_counter = instruction.true_target().address();
    else
        program_counter = instruction.false_target().address();
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_JumpUndefined(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::JumpUndefined const*>(&bytecode[program_counter]);
    if (get(instruction.condition()).is_undefined())
        program_counter = instruction.true_target().address();
    else
        program_counter = instruction.false_target().address();
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_EnterUnwindContext(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::EnterUnwindContext const*>(&bytecode[program_counter]);
    enter_unwind_context();
    program_counter = instruction.entry_point().address();
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_ContinuePendingUnwind(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::ContinuePendingUnwind const*>(&bytecode[program_counter]);
    if (auto exception = reg(Register::exception()); !exception.is_special_empty_value()) {
        if (handle_exception(program_counter, exception) == Interpreter::HandleExceptionResponse::ExitFromExecutable)
            return;
        DISPATCH_NEXT();
    }
    if (!saved_return_value().is_special_empty_value()) {
        do_return(saved_return_value());
        if (auto handlers = current_executable().exception_handlers_for_offset(program_counter); handlers.has_value()) {
            if (auto finalizer = handlers.value().finalizer_offset; finalizer.has_value()) {
                VERIFY(!running_execution_context().unwind_contexts.is_empty());
                auto& unwind_context = running_execution_context().unwind_contexts.last();
                VERIFY(unwind_context.executable == &current_executable());
                reg(Register::saved_return_value()) = reg(Register::return_value());
                reg(Register::return_value()) = js_special_empty_value();
                program_counter = finalizer.value();
                // the unwind_context will be pop'ed when entering the finally block
                DISPATCH_NEXT();
            }
        }
        return;
    }
    auto const old_scheduled_jump = running_execution_context().previously_scheduled_jumps.take_last();
    if (m_scheduled_jump.has_value()) {
        program_counter = m_scheduled_jump.value();
        m_scheduled_jump = {};
    } else {
        program_counter = instruction.resume_target().address();
        // set the scheduled jump to the old value if we continue
        // where we left it
        m_scheduled_jump = old_scheduled_jump;
    }
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_ScheduleJump(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::ScheduleJump const*>(&bytecode[program_counter]);
    m_scheduled_jump = instruction.target().address();
    auto finalizer = current_executable().exception_handlers_for_offset(program_counter).value().finalizer_offset;
    VERIFY(finalizer.has_value());
    program_counter = finalizer.value();
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_Await(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::Await const*>(&bytecode[program_counter]);
    instruction.execute_impl(*this);
}

FLATTEN void Interpreter::handle_Return(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::Return const*>(&bytecode[program_counter]);
    instruction.execute_impl(*this);
}

FLATTEN void Interpreter ::handle_PrepareYield(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op ::PrepareYield const*>(&bytecode[program_counter]);
    instruction.execute_impl(*this);
    INCREMENT_PROGRAM_COUNTER(PrepareYield);
    DISPATCH_NEXT();
}

FLATTEN void Interpreter::handle_Yield(u8 const* bytecode, size_t& program_counter)
{
    auto& instruction = *reinterpret_cast<Op::Yield const*>(&bytecode[program_counter]);
    instruction.execute_impl(*this);
    // Note: A `yield` statement will not go through a finally statement,
    //       hence we need to set a flag to not do so,
    //       but we generate a Yield Operation in the case of returns in
    //       generators as well, so we need to check if it will actually
    //       continue or is a `return` in disguise
}

namespace Op {

void Await::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto yielded_value = interpreter.get(m_argument).is_special_empty_value() ? js_undefined() : interpreter.get(m_argument);
    // FIXME: If we get a pointer, which is not accurately representable as a double
    //        will cause this to explode
    auto continuation_value = Value(m_continuation_label.address());
    auto result = interpreter.vm().heap().allocate<GeneratorResult>(yielded_value, continuation_value, true);
    interpreter.do_return(result);
}

void Return::execute_impl(Bytecode::Interpreter& interpreter) const
{
    interpreter.do_return(interpreter.get(m_value));
}

void PrepareYield::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto value = interpreter.get(m_value).is_special_empty_value() ? js_undefined() : interpreter.get(m_value);
    interpreter.set(m_dest, interpreter.do_yield(value, {}));
}

void Yield::execute_impl(Bytecode::Interpreter& interpreter) const
{
    auto yielded_value = interpreter.get(m_value).is_special_empty_value() ? js_undefined() : interpreter.get(m_value);
    interpreter.do_return(
        interpreter.do_yield(yielded_value, m_continuation_label));
}

}

}
