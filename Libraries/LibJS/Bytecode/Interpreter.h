/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Bytecode/Label.h>
#include <LibJS/Bytecode/Register.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/FunctionKind.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Bytecode {

class InstructionStreamIterator;

class JS_API Interpreter {
public:
    explicit Interpreter(VM&);
    ~Interpreter();

    [[nodiscard]] Realm& realm() { return *m_running_execution_context->realm; }
    [[nodiscard]] Object& global_object() { return *m_running_execution_context->global_object; }
    [[nodiscard]] DeclarativeEnvironment& global_declarative_environment() { return *m_running_execution_context->global_declarative_environment; }
    VM& vm() { return m_vm; }
    VM const& vm() const { return m_vm; }

    ThrowCompletionOr<Value> run(Script&, GC::Ptr<Environment> lexical_environment_override = nullptr);
    ThrowCompletionOr<Value> run(SourceTextModule&);

    ThrowCompletionOr<Value> run(ExecutionContext& context, Executable& executable, Optional<size_t> entry_point = {}, Value initial_accumulator_value = js_special_empty_value())
    {
        auto result_and_return_register = run_executable(context, executable, entry_point, initial_accumulator_value);
        return move(result_and_return_register.value);
    }

    struct ResultAndReturnRegister {
        ThrowCompletionOr<Value> value;
        Value return_register_value;
    };
    ResultAndReturnRegister run_executable(ExecutionContext&, Executable&, Optional<size_t> entry_point, Value initial_accumulator_value = js_special_empty_value());

    ALWAYS_INLINE Value& accumulator() { return reg(Register::accumulator()); }
    ALWAYS_INLINE Value& saved_return_value() { return reg(Register::saved_return_value()); }
    Value& reg(Register const& r)
    {
        return m_running_execution_context->registers_and_constants_and_locals_arguments.data()[r.index()];
    }
    Value reg(Register const& r) const
    {
        return m_running_execution_context->registers_and_constants_and_locals_arguments.data()[r.index()];
    }

    [[nodiscard]] Value get(Operand) const;
    void set(Operand, Value);

    Value do_yield(Value value, Optional<Label> continuation);
    void do_return(Value value)
    {
        reg(Register::return_value()) = value;
        reg(Register::exception()) = js_special_empty_value();
    }

    void enter_unwind_context();
    void leave_unwind_context();
    void catch_exception(Operand dst);
    void restore_scheduled_jump();
    void leave_finally();

    void enter_object_environment(Object&);

    Executable& current_executable() { return *m_running_execution_context->executable; }
    Executable const& current_executable() const { return *m_running_execution_context->executable; }

    ExecutionContext& running_execution_context() { return *m_running_execution_context; }

    [[nodiscard]] Utf16FlyString const& get_identifier(IdentifierTableIndex) const;
    [[nodiscard]] Optional<Utf16FlyString const&> get_identifier(Optional<IdentifierTableIndex> index) const
    {
        if (!index.has_value())
            return {};
        return get_identifier(*index);
    }

private:
    void run_bytecode(size_t entry_point);

    enum class HandleExceptionResponse {
        ExitFromExecutable,
        ContinueInThisExecutable,
    };
    [[nodiscard]] HandleExceptionResponse handle_exception(u32& program_counter, Value exception);

    VM& m_vm;
    ExecutionContext* m_running_execution_context { nullptr };
};

JS_API extern bool g_dump_bytecode;

ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM&, ASTNode const&, JS::FunctionKind kind, Utf16FlyString const& name);
ThrowCompletionOr<GC::Ref<Bytecode::Executable>> compile(VM&, ECMAScriptFunctionObject const&);

}
