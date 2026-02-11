/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Bytecode/BuiltinAbstractOperationsEnabled.h>
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
    Interpreter();
    ~Interpreter();

    [[nodiscard]] Realm& realm() { return *m_running_execution_context->realm; }
    [[nodiscard]] Object& global_object() { return *m_running_execution_context->global_object; }
    [[nodiscard]] DeclarativeEnvironment& global_declarative_environment() { return *m_running_execution_context->global_declarative_environment; }
    static VM& vm() { return VM::the(); }

    ThrowCompletionOr<Value> run(Script&, GC::Ptr<Environment> lexical_environment_override = nullptr);
    ThrowCompletionOr<Value> run(SourceTextModule&);

    ThrowCompletionOr<Value> run_executable(ExecutionContext&, Executable&, Optional<size_t> entry_point);

    ThrowCompletionOr<Value> run_executable(ExecutionContext& context, Executable& executable, Optional<size_t> entry_point, Value initial_accumulator_value)
    {
        context.registers_and_constants_and_locals_and_arguments_span()[0] = initial_accumulator_value;
        return run_executable(context, executable, entry_point);
    }

    ALWAYS_INLINE Value& accumulator() { return reg(Register::accumulator()); }
    Value& reg(Register const& r)
    {
        return m_running_execution_context->registers_and_constants_and_locals_and_arguments()[r.index()];
    }
    Value reg(Register const& r) const
    {
        return m_running_execution_context->registers_and_constants_and_locals_and_arguments()[r.index()];
    }

    [[nodiscard]] Value get(Operand) const;
    void set(Operand, Value);

    Value do_yield(Value value, Optional<Label> continuation);
    void do_return(Value value)
    {
        if (value.is_special_empty_value())
            value = js_undefined();
        reg(Register::return_value()) = value;
        reg(Register::exception()) = js_special_empty_value();
    }

    void catch_exception(Operand dst);

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

    [[nodiscard]] PropertyKey const& get_property_key(PropertyKeyTableIndex) const;

private:
    void run_bytecode(size_t entry_point);

    enum class HandleExceptionResponse {
        ExitFromExecutable,
        ContinueInThisExecutable,
    };
    [[nodiscard]] COLD HandleExceptionResponse handle_exception(u32& program_counter, Value exception);

    ExecutionContext* m_running_execution_context { nullptr };
};

JS_API extern bool g_dump_bytecode;

GC::Ref<Bytecode::Executable> compile(VM&, ASTNode const&, JS::FunctionKind kind, Utf16FlyString const& name);
GC::Ref<Bytecode::Executable> compile(VM&, GC::Ref<SharedFunctionInstanceData const>, BuiltinAbstractOperationsEnabled builtin_abstract_operations_enabled);

}
