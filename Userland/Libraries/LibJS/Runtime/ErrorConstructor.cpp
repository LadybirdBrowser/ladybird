/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ErrorConstructor);

ErrorConstructor::ErrorConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Error.as_string(), realm.intrinsics().function_prototype())
{
}

void ErrorConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 20.5.2.1 Error.prototype, https://tc39.es/ecma262/#sec-error.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().error_prototype(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.isError, is_error, 1, attr);
}

// 20.5.1.1 Error ( message [ , options ] ), https://tc39.es/ecma262/#sec-error-message
ThrowCompletionOr<Value> ErrorConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
    return TRY(construct(*this));
}

// 20.5.1.1 Error ( message [ , options ] ), https://tc39.es/ecma262/#sec-error-message
ThrowCompletionOr<NonnullGCPtr<Object>> ErrorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto message = vm.argument(0);
    auto options = vm.argument(1);

    // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%Error.prototype%", « [[ErrorData]] »).
    auto error = TRY(ordinary_create_from_constructor<Error>(vm, new_target, &Intrinsics::error_prototype));

    // 3. If message is not undefined, then
    if (!message.is_undefined()) {
        // a. Let msg be ? ToString(message).
        auto msg = TRY(message.to_string(vm));

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
        error->create_non_enumerable_data_property_or_throw(vm.names.message, PrimitiveString::create(vm, move(msg)));
    }

    // 4. Perform ? InstallErrorCause(O, options).
    TRY(error->install_error_cause(options));

    // 5. Return O.
    return error;
}

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType)                                    \
    JS_DEFINE_ALLOCATOR(ConstructorName);                                                                                   \
    ConstructorName::ConstructorName(Realm& realm)                                                                          \
        : NativeFunction(realm.vm().names.ClassName.as_string(), realm.intrinsics().error_constructor())                    \
    {                                                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    void ConstructorName::initialize(Realm& realm)                                                                          \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
        Base::initialize(realm);                                                                                            \
                                                                                                                            \
        /* 20.5.6.2.1 NativeError.prototype, https://tc39.es/ecma262/#sec-nativeerror.prototype */                          \
        define_direct_property(vm.names.prototype, realm.intrinsics().snake_name##_prototype(), 0);                         \
                                                                                                                            \
        define_direct_property(vm.names.length, Value(1), Attribute::Configurable);                                         \
    }                                                                                                                       \
                                                                                                                            \
    ConstructorName::~ConstructorName() = default;                                                                          \
                                                                                                                            \
    /* 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror */                        \
    ThrowCompletionOr<Value> ConstructorName::call()                                                                        \
    {                                                                                                                       \
        /* 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget. */   \
        return TRY(construct(*this));                                                                                       \
    }                                                                                                                       \
                                                                                                                            \
    /* 20.5.6.1.1 NativeError ( message [ , options ] ), https://tc39.es/ecma262/#sec-nativeerror */                        \
    ThrowCompletionOr<NonnullGCPtr<Object>> ConstructorName::construct(FunctionObject& new_target)                          \
    {                                                                                                                       \
        auto& vm = this->vm();                                                                                              \
                                                                                                                            \
        auto message = vm.argument(0);                                                                                      \
        auto options = vm.argument(1);                                                                                      \
                                                                                                                            \
        /* 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%NativeError.prototype%", « [[ErrorData]] »). */       \
        auto error = TRY(ordinary_create_from_constructor<ClassName>(vm, new_target, &Intrinsics::snake_name##_prototype)); \
                                                                                                                            \
        /* 3. If message is not undefined, then */                                                                          \
        if (!message.is_undefined()) {                                                                                      \
            /* a. Let msg be ? ToString(message). */                                                                        \
            auto msg = TRY(message.to_string(vm));                                                                          \
                                                                                                                            \
            /* b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg). */                                     \
            error->create_non_enumerable_data_property_or_throw(vm.names.message, PrimitiveString::create(vm, move(msg)));  \
        }                                                                                                                   \
                                                                                                                            \
        /* 4. Perform ? InstallErrorCause(O, options). */                                                                   \
        TRY(error->install_error_cause(options));                                                                           \
                                                                                                                            \
        /* 5. Return O. */                                                                                                  \
        return error;                                                                                                       \
    }

JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE

// 20.5.2.1 Error.isError ( arg ), https://tc39.es/proposal-is-error/#sec-error.iserror
JS_DEFINE_NATIVE_FUNCTION(ErrorConstructor::is_error)
{
    auto arg = vm.argument(0);

    // 1. Return IsError(arg).
    return Value(arg.is_error());
}

}
