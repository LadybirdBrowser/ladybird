/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Bytecode/BuiltinAbstractOperationsEnabled.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Runtime/AsyncFunctionDriverWrapper.h>
#include <LibJS/Runtime/AsyncGenerator.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/NativeJavaScriptBackedFunction.h>
#include <LibJS/RustIntegration.h>

namespace JS {

GC_DEFINE_ALLOCATOR(NativeJavaScriptBackedFunction);

// 10.3.3 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
GC::Ref<NativeJavaScriptBackedFunction> NativeJavaScriptBackedFunction::create(Realm& realm, GC::Ref<SharedFunctionInstanceData> shared_data, PropertyKey const& name, i32 length)
{
    // 1. If realm is not present, set realm to the current Realm Record.
    // 2. If prototype is not present, set prototype to realm.[[Intrinsics]].[[%Function.prototype%]].
    auto prototype = realm.intrinsics().function_prototype();

    // 3. Let internalSlotsList be a List containing the names of all the internal slots that 10.3 requires for the built-in function object that is about to be created.
    // 4. Append to internalSlotsList the elements of additionalInternalSlotsList.

    // 5. Let func be a new built-in function object that, when called, performs the action described by behaviour using the provided arguments as the values of the corresponding parameters specified by behaviour. The new function object has internal slots whose names are the elements of internalSlotsList, and an [[InitialName]] internal slot.
    // 6. Set func.[[Prototype]] to prototype.
    // 7. Set func.[[Extensible]] to true.
    // 8. Set func.[[Realm]] to realm.
    // 9. Set func.[[InitialName]] to null.
    auto function = realm.create<NativeJavaScriptBackedFunction>(shared_data, *prototype);

    function->unsafe_set_shape(realm.intrinsics().native_function_shape());

    // 10. Perform SetFunctionLength(func, length).
    function->put_direct(realm.intrinsics().native_function_length_offset(), Value { length });

    // 11. If prefix is not present, then
    //     a. Perform SetFunctionName(func, name).
    // 12. Else,
    //     a. Perform SetFunctionName(func, name, prefix).
    function->put_direct(realm.intrinsics().native_function_name_offset(), function->make_function_name(name, OptionalNone {}));

    // 13. Return func.
    return function;
}

NativeJavaScriptBackedFunction::NativeJavaScriptBackedFunction(GC::Ref<SharedFunctionInstanceData> shared_function_instance_data, Object& prototype)
    : NativeFunction(shared_function_instance_data->m_name, prototype)
    , m_shared_function_instance_data(shared_function_instance_data)
{
}

void NativeJavaScriptBackedFunction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_shared_function_instance_data);
}

void NativeJavaScriptBackedFunction::get_stack_frame_size(size_t& registers_and_locals_count, size_t& constants_count, size_t& argument_count)
{
    auto& bytecode_executable = this->bytecode_executable();
    registers_and_locals_count = bytecode_executable.registers_and_locals_count;
    constants_count = bytecode_executable.constants.size();
    argument_count = max(argument_count, m_shared_function_instance_data->m_function_length);
}

ThrowCompletionOr<Value> NativeJavaScriptBackedFunction::call()
{
    auto& vm = this->vm();

    auto result = TRY(vm.bytecode_interpreter().run_executable(vm.running_execution_context(), bytecode_executable(), {}));

    auto kind = this->kind();
    if (kind == FunctionKind::Normal)
        return result;

    auto& realm = *vm.current_realm();
    if (kind == FunctionKind::AsyncGenerator)
        return AsyncGenerator::create(realm, result, GC::Ref { *this }, vm.running_execution_context().copy());

    auto generator_object = GeneratorObject::create(realm, result, GC::Ref { *this }, vm.running_execution_context().copy());

    // NOTE: Async functions are entirely transformed to generator functions, and wrapped in a custom driver that returns a promise
    //       See AwaitExpression::generate_bytecode() for the transformation.
    if (kind == FunctionKind::Async)
        return AsyncFunctionDriverWrapper::create(realm, generator_object);

    VERIFY(kind == FunctionKind::Generator);
    return generator_object;
}

Bytecode::Executable& NativeJavaScriptBackedFunction::bytecode_executable()
{
    auto& executable = m_shared_function_instance_data->m_executable;
    if (!executable) {
        auto rust_executable = RustIntegration::compile_function(vm(), *m_shared_function_instance_data, true);
        if (rust_executable) {
            executable = rust_executable;
            executable->name = m_shared_function_instance_data->m_name;
            if (Bytecode::g_dump_bytecode)
                executable->dump();
        } else {
            executable = Bytecode::compile(vm(), m_shared_function_instance_data, Bytecode::BuiltinAbstractOperationsEnabled::Yes);
        }
        m_shared_function_instance_data->clear_compile_inputs();
    }

    return *executable;
}

FunctionKind NativeJavaScriptBackedFunction::kind() const
{
    return m_shared_function_instance_data->m_kind;
}

ThisMode NativeJavaScriptBackedFunction::this_mode() const
{
    return m_shared_function_instance_data->m_this_mode;
}

bool NativeJavaScriptBackedFunction::function_environment_needed() const
{
    return m_shared_function_instance_data->m_function_environment_needed;
}

size_t NativeJavaScriptBackedFunction::function_environment_bindings_count() const
{
    return m_shared_function_instance_data->m_function_environment_bindings_count;
}

bool NativeJavaScriptBackedFunction::is_strict_mode() const
{
    return m_shared_function_instance_data->m_strict;
}

}
