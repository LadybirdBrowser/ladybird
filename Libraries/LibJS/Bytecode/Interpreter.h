/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Bytecode {

class InstructionStreamIterator;

class Interpreter {
public:
    explicit Interpreter(VM&);
    ~Interpreter();

    [[nodiscard]] Realm& realm() { return *m_realm; }
    [[nodiscard]] Object& global_object() { return *m_global_object; }
    [[nodiscard]] DeclarativeEnvironment& global_declarative_environment() { return *m_global_declarative_environment; }
    VM& vm() { return m_vm; }
    VM const& vm() const { return m_vm; }

    ThrowCompletionOr<Value> run(Script&, GC::Ptr<Environment> lexical_environment_override = nullptr);
    ThrowCompletionOr<Value> run(SourceTextModule&);

    ThrowCompletionOr<Value> run(Bytecode::Executable& executable, Optional<size_t> entry_point = {}, Value initial_accumulator_value = {})
    {
        auto result_and_return_register = run_executable(executable, entry_point, initial_accumulator_value);
        return move(result_and_return_register.value);
    }

    struct ResultAndReturnRegister {
        ThrowCompletionOr<Value> value;
        Value return_register_value;
    };
    ResultAndReturnRegister run_executable(Bytecode::Executable&, Optional<size_t> entry_point, Value initial_accumulator_value = {});

    ALWAYS_INLINE Value& accumulator() { return reg(Register::accumulator()); }
    ALWAYS_INLINE Value& saved_return_value() { return reg(Register::saved_return_value()); }
    Value& reg(Register const& r)
    {
        return m_registers_and_constants_and_locals.data()[r.index()];
    }
    Value reg(Register const& r) const
    {
        return m_registers_and_constants_and_locals.data()[r.index()];
    }

    [[nodiscard]] Value get(Operand) const;
    void set(Operand, Value);

    Value do_yield(Value value, Optional<Label> continuation);
    void do_return(Value value)
    {
        reg(Register::return_value()) = value;
        reg(Register::exception()) = {};
    }

    void enter_unwind_context();
    void leave_unwind_context();
    void catch_exception(Operand dst);
    void restore_scheduled_jump();
    void leave_finally();

    void enter_object_environment(Object&);

    Executable& current_executable() { return *m_current_executable; }
    Executable const& current_executable() const { return *m_current_executable; }
    Optional<size_t> program_counter() const { return m_program_counter; }
    Span<Value> allocate_argument_values(size_t argument_count)
    {
        m_argument_values_buffer.resize_and_keep_capacity(argument_count);
        return m_argument_values_buffer.span();
    }

    ExecutionContext& running_execution_context() { return *m_running_execution_context; }

private:
    void run_bytecode(size_t entry_point);

    enum class HandleExceptionResponse {
        ExitFromExecutable,
        ContinueInThisExecutable,
    };
    [[nodiscard]] HandleExceptionResponse handle_exception(size_t& program_counter, Value exception);

    VM& m_vm;
    Optional<size_t> m_scheduled_jump;
    GC::Ptr<Executable> m_current_executable { nullptr };
    GC::Ptr<Realm> m_realm { nullptr };
    GC::Ptr<Object> m_global_object { nullptr };
    GC::Ptr<DeclarativeEnvironment> m_global_declarative_environment { nullptr };
    Optional<size_t&> m_program_counter;
    Span<Value> m_arguments;
    Span<Value> m_registers_and_constants_and_locals;
    Vector<Value> m_argument_values_buffer;
    ExecutionContext* m_running_execution_context { nullptr };
};

extern bool g_dump_bytecode;

ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM&, ASTNode const&, JS::FunctionKind kind, FlyString const& name);
ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM&, ECMAScriptFunctionObject const&);

}
