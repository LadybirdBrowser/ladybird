/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/Executable.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/FunctionObject.h>

namespace JS {

class ExecutionContextAllocator {
public:
    NonnullOwnPtr<ExecutionContext> allocate()
    {
        if (m_execution_contexts.is_empty())
            return adopt_own(*new ExecutionContext);
        void* slot = m_execution_contexts.take_last();
        return adopt_own(*new (slot) ExecutionContext);
    }
    void deallocate(void* ptr)
    {
        m_execution_contexts.append(ptr);
    }

private:
    Vector<void*> m_execution_contexts;
};

static NeverDestroyed<ExecutionContextAllocator> s_execution_context_allocator;

NonnullOwnPtr<ExecutionContext> ExecutionContext::create()
{
    return s_execution_context_allocator->allocate();
}

void ExecutionContext::operator delete(void* ptr)
{
    s_execution_context_allocator->deallocate(ptr);
}

ExecutionContext::ExecutionContext()
{
}

ExecutionContext::~ExecutionContext()
{
}

NonnullOwnPtr<ExecutionContext> ExecutionContext::copy() const
{
    auto copy = create();
    copy->function = function;
    copy->realm = realm;
    copy->script_or_module = script_or_module;
    copy->lexical_environment = lexical_environment;
    copy->variable_environment = variable_environment;
    copy->private_environment = private_environment;
    copy->program_counter = program_counter;
    copy->function_name = function_name;
    copy->this_value = this_value;
    copy->is_strict_mode = is_strict_mode;
    copy->executable = executable;
    copy->arguments = arguments;
    copy->passed_argument_count = passed_argument_count;
    copy->registers_and_constants_and_locals = registers_and_constants_and_locals;
    copy->unwind_contexts = unwind_contexts;
    copy->saved_lexical_environments = saved_lexical_environments;
    copy->previously_scheduled_jumps = previously_scheduled_jumps;
    return copy;
}

void ExecutionContext::visit_edges(Cell::Visitor& visitor)
{
    visitor.visit(function);
    visitor.visit(realm);
    visitor.visit(variable_environment);
    visitor.visit(lexical_environment);
    visitor.visit(private_environment);
    visitor.visit(context_owner);
    visitor.visit(this_value);
    visitor.visit(executable);
    visitor.visit(function_name);
    visitor.visit(arguments);
    visitor.visit(registers_and_constants_and_locals);
    for (auto& context : unwind_contexts) {
        visitor.visit(context.lexical_environment);
    }
    visitor.visit(saved_lexical_environments);
    script_or_module.visit(
        [](Empty) {},
        [&](auto& script_or_module) {
            visitor.visit(script_or_module);
        });
}

}
