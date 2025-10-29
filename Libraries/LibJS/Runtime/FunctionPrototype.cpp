/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BoundFunction.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/ShadowRealm.h>

namespace JS {

GC_DEFINE_ALLOCATOR(FunctionPrototype);

FunctionPrototype::FunctionPrototype(Realm& realm)
    : FunctionObject(realm.intrinsics().object_prototype())
{
}

void FunctionPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.apply, apply, 2, attr);
    define_native_function(realm, vm.names.bind, bind, 1, attr);
    define_native_function(realm, vm.names.call, call, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.well_known_symbol_has_instance(), symbol_has_instance, 1, 0, Bytecode::Builtin::OrdinaryHasInstance);
    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
    define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);
}

ThrowCompletionOr<Value> FunctionPrototype::internal_call(ExecutionContext&, Value)
{
    // The Function prototype object:
    // - accepts any arguments and returns undefined when invoked.
    return js_undefined();
}

// 20.2.3.1 Function.prototype.apply ( thisArg, argArray ), https://tc39.es/ecma262/#sec-function.prototype.apply
JS_DEFINE_NATIVE_FUNCTION(FunctionPrototype::apply)
{
    // 1. Let func be the this value.
    auto function_value = vm.this_value();

    // 2. If IsCallable(func) is false, throw a TypeError exception.
    if (!function_value.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, function_value.to_string_without_side_effects());

    auto& function = static_cast<FunctionObject&>(function_value.as_object());

    auto this_arg = vm.argument(0);
    auto arg_array = vm.argument(1);

    // 3. If argArray is undefined or null, then
    if (arg_array.is_nullish()) {
        // FIXME: a. Perform PrepareForTailCall().

        // b. Return ? Call(func, thisArg).
        return TRY(JS::call(vm, function, this_arg));
    }

    // NOTE: Do the check performed by CreateListFromArrayLike here, so we could avoid branching in optimized code path.
    if (!arg_array.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, arg_array.to_string_without_side_effects());

    // OPTIMIZATION: If argArray has a simple indexed storage without holes and doesn't interfere with indexed property access,
    //               we can skip CreateListFromArrayLike and directly use the storage elements.
    auto& arg_array_object = arg_array.as_object();
    auto* storage = arg_array_object.indexed_properties().storage();
    if (!arg_array_object.may_interfere_with_indexed_property_access() && storage && storage->is_simple_storage()) {
        auto length = TRY(length_of_array_like(vm, arg_array_object));
        auto const* simple_storage = static_cast<SimpleIndexedPropertyStorage*>(storage);
        auto storage_elements = simple_storage->elements().span();
        if (!simple_storage->has_empty_elements() && storage_elements.size() >= length)
            return TRY(JS::call(vm, function, this_arg, storage_elements.slice(0, length)));
    }

    // 4. Let argList be ? CreateListFromArrayLike(argArray).
    auto arguments = TRY(create_list_from_array_like(vm, arg_array));

    // FIXME: 5. Perform PrepareForTailCall().

    // 6. Return ? Call(func, thisArg, argList).
    return TRY(JS::call(vm, function, this_arg, arguments.span()));
}

// 20.2.3.2 Function.prototype.bind ( thisArg, ...args ), https://tc39.es/ecma262/#sec-function.prototype.bind
// 3.1.2.1 Function.prototype.bind ( thisArg, ...args ), https://tc39.es/proposal-shadowrealm/#sec-function.prototype.bind
JS_DEFINE_NATIVE_FUNCTION(FunctionPrototype::bind)
{
    auto& realm = *vm.current_realm();

    auto this_argument = vm.argument(0);

    // 1. Let Target be the this value.
    auto target_value = vm.this_value();

    // 2. If IsCallable(Target) is false, throw a TypeError exception.
    if (!target_value.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, target_value.to_string_without_side_effects());

    auto& target = static_cast<FunctionObject&>(target_value.as_object());

    Vector<Value> arguments;
    if (vm.argument_count() > 1) {
        arguments.append(vm.running_execution_context().arguments.slice(1).data(), vm.argument_count() - 1);
    }

    // 3. Let F be ? BoundFunctionCreate(Target, thisArg, args).
    auto function = TRY(BoundFunction::create(realm, target, this_argument, move(arguments)));

    // 4. Let argCount be the number of elements in args.
    auto arg_count = vm.argument_count() > 0 ? vm.argument_count() - 1 : 0;

    // 5. Perform ? CopyNameAndLength(F, Target, "bound", argCount).
    TRY(copy_name_and_length(vm, *function, target, "bound"sv, arg_count));

    // 6. Return F.
    return function;
}

// 20.2.3.3 Function.prototype.call ( thisArg, ...args ), https://tc39.es/ecma262/#sec-function.prototype.call
JS_DEFINE_NATIVE_FUNCTION(FunctionPrototype::call)
{
    // 1. Let func be the this value.
    auto function_value = vm.this_value();

    // 2. If IsCallable(func) is false, throw a TypeError exception.
    if (!function_value.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, function_value.to_string_without_side_effects());

    auto& function = static_cast<FunctionObject&>(function_value.as_object());

    // FIXME: 3. Perform PrepareForTailCall().

    auto this_arg = vm.argument(0);
    auto args = vm.argument_count() > 1 ? vm.running_execution_context().arguments.slice(1) : ReadonlySpan<Value> {};

    // 4. Return ? Call(func, thisArg, args).
    return TRY(JS::call(vm, function, this_arg, args));
}

// 20.2.3.5 Function.prototype.toString ( ), https://tc39.es/ecma262/#sec-function.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(FunctionPrototype::to_string)
{
    // 1. Let func be the this value.
    auto function_value = vm.this_value();

    // OPTIMIZATION: If func is not a function, bail out early. The order of this step is not observable.
    if (!function_value.is_function()) {
        // 5. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Function");
    }

    auto& function = function_value.as_function();

    // 2. If Type(func) is Object and func has a [[SourceText]] internal slot and func.[[SourceText]] is a sequence of Unicode code points and HostHasSourceTextAvailable(func) is true, then
    if (is<ECMAScriptFunctionObject>(function)) {
        // a. Return CodePointsToString(func.[[SourceText]]).
        return PrimitiveString::create(vm, static_cast<ECMAScriptFunctionObject&>(function).source_text());
    }

    // 3. If func is a built-in function object, return an implementation-defined String source code representation of func. The representation must have the syntax of a NativeFunction. Additionally, if func has an [[InitialName]] internal slot and func.[[InitialName]] is a String, the portion of the returned String that would be matched by NativeFunctionAccessor[opt] PropertyName must be the value of func.[[InitialName]].
    if (is<NativeFunction>(function)) {
        // NOTE: once we remove name(), the fallback here can simply be an empty string.
        auto const& native_function = static_cast<NativeFunction&>(function);
        auto const name = native_function.initial_name().value_or(native_function.name());
        return PrimitiveString::create(vm, ByteString::formatted("function {}() {{ [native code] }}", name));
    }

    // 4. If Type(func) is Object and IsCallable(func) is true, return an implementation-defined String source code representation of func. The representation must have the syntax of a NativeFunction.
    // NOTE: ProxyObject, BoundFunction, WrappedFunction
    return PrimitiveString::create(vm, "function () { [native code] }"_string);
}

// 20.2.3.6 Function.prototype [ @@hasInstance ] ( V ), https://tc39.es/ecma262/#sec-function.prototype-@@hasinstance
JS_DEFINE_NATIVE_FUNCTION(FunctionPrototype::symbol_has_instance)
{
    // 1. Let F be the this value.
    // 2. Return ? OrdinaryHasInstance(F, V).
    return TRY(ordinary_has_instance(vm, vm.argument(0), vm.this_value()));
}

Utf16String FunctionPrototype::name_for_call_stack() const
{
    return "(Function.prototype)"_utf16;
}

}
