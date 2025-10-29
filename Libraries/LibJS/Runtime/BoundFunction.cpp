/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BoundFunction.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(BoundFunction);

// 10.4.1.3 BoundFunctionCreate ( targetFunction, boundThis, boundArgs ), https://tc39.es/ecma262/#sec-boundfunctioncreate
ThrowCompletionOr<GC::Ref<BoundFunction>> BoundFunction::create(Realm& realm, FunctionObject& target_function, Value bound_this, Vector<Value> bound_arguments)
{
    // 1. Let proto be ? targetFunction.[[GetPrototypeOf]]().
    auto* prototype = TRY(target_function.internal_get_prototype_of());

    // 2. Let internalSlotsList be the list-concatenation of « [[Prototype]], [[Extensible]] » and the internal slots listed in Table 34.
    // 3. Let obj be MakeBasicObject(internalSlotsList).
    // 4. Set obj.[[Prototype]] to proto.
    // 5. Set obj.[[Call]] as described in 10.4.1.1.
    // 6. If IsConstructor(targetFunction) is true, then
    //    a. Set obj.[[Construct]] as described in 10.4.1.2.
    // 7. Set obj.[[BoundTargetFunction]] to targetFunction.
    // 8. Set obj.[[BoundThis]] to boundThis.
    // 9. Set obj.[[BoundArguments]] to boundArgs.
    auto object = realm.create<BoundFunction>(realm, target_function, bound_this, move(bound_arguments), prototype);

    // 10. Return obj.
    return object;
}

BoundFunction::BoundFunction(Realm& realm, FunctionObject& bound_target_function, Value bound_this, Vector<Value> bound_arguments, Object* prototype)
    : FunctionObject(realm, prototype)
    , m_bound_target_function(&bound_target_function)
    , m_bound_this(bound_this)
    , m_bound_arguments(move(bound_arguments))
{
}

// 10.4.1.1 [[Call]] ( thisArgument, argumentsList ), https://tc39.es/ecma262/#sec-bound-function-exotic-objects-call-thisargument-argumentslist
ThrowCompletionOr<Value> BoundFunction::internal_call(ExecutionContext& callee_context, [[maybe_unused]] Value this_argument)
{
    // 1. Let target be F.[[BoundTargetFunction]].
    auto& target = *m_bound_target_function;

    // 2. Let boundThis be F.[[BoundThis]].
    auto bound_this = m_bound_this;

    // 3. Let boundArgs be F.[[BoundArguments]].
    auto& bound_args = m_bound_arguments;

    // 4. Let args be the list-concatenation of boundArgs and argumentsList.
    auto* argument_values = callee_context.arguments.data();

    for (ssize_t i = static_cast<ssize_t>(callee_context.arguments.size()) - 1; i >= static_cast<ssize_t>(bound_args.size()); --i)
        argument_values[i] = argument_values[i - bound_args.size()];
    for (size_t i = 0; i < bound_args.size(); ++i)
        argument_values[i] = bound_args[i];

    callee_context.passed_argument_count += bound_args.size();

    // 5. Return ? Call(target, boundThis, args).
    return target.internal_call(callee_context, bound_this);
}

// 10.4.1.2 [[Construct]] ( argumentsList, newTarget ), https://tc39.es/ecma262/#sec-bound-function-exotic-objects-construct-argumentslist-newtarget
ThrowCompletionOr<GC::Ref<Object>> BoundFunction::internal_construct(ExecutionContext& callee_context, FunctionObject& new_target)
{
    // 1. Let target be F.[[BoundTargetFunction]].
    auto& target = *m_bound_target_function;

    // 2. Assert: IsConstructor(target) is true.
    VERIFY(Value(&target).is_constructor());

    // 3. Let boundArgs be F.[[BoundArguments]].
    auto& bound_args = m_bound_arguments;

    // 4. Let args be the list-concatenation of boundArgs and argumentsList.
    auto* argument_values = callee_context.arguments.data();

    for (ssize_t i = static_cast<ssize_t>(callee_context.arguments.size()) - 1; i >= static_cast<ssize_t>(bound_args.size()); --i)
        argument_values[i] = argument_values[i - bound_args.size()];
    for (size_t i = 0; i < bound_args.size(); ++i)
        argument_values[i] = bound_args[i];

    callee_context.passed_argument_count += bound_args.size();

    // 5. If SameValue(F, newTarget) is true, set newTarget to target.
    auto* final_new_target = &new_target;
    if (this == &new_target)
        final_new_target = &target;

    // 6. Return ? Construct(target, args, newTarget).
    return target.internal_construct(callee_context, *final_new_target);
}

void BoundFunction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_bound_target_function);
    visitor.visit(m_bound_this);
    visitor.visit(m_bound_arguments);
}

ThrowCompletionOr<void> BoundFunction::get_stack_frame_size(size_t& registers_and_constants_and_locals_count, size_t& argument_count)
{
    TRY(m_bound_target_function->get_stack_frame_size(registers_and_constants_and_locals_count, argument_count));
    argument_count += m_bound_arguments.size();
    return {};
}

Utf16String BoundFunction::name_for_call_stack() const
{
    return m_bound_target_function->name_for_call_stack();
}

}
