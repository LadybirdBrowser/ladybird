/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/SuppressedError.h>
#include <LibJS/Runtime/SuppressedErrorConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SuppressedErrorConstructor);

SuppressedErrorConstructor::SuppressedErrorConstructor(Realm& realm)
    : NativeFunction(static_cast<Object&>(realm.intrinsics().error_constructor()))
{
}

void SuppressedErrorConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 10.1.4.2.1 SuppressedError.prototype, https://tc39.es/proposal-explicit-resource-management/#sec-suppressederror.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().suppressed_error_prototype(), 0);

    define_direct_property(vm.names.length, Value(3), Attribute::Configurable);
}

// 10.1.4.1.1 SuppressedError ( error, suppressed, message [ , options ] ), https://tc39.es/proposal-explicit-resource-management/#sec-suppressederror
ThrowCompletionOr<Value> SuppressedErrorConstructor::call()
{
    // 1. If NewTarget is undefined, let newTarget be the active function object; else let newTarget be NewTarget.
    return TRY(construct(*this));
}

// 10.1.4.1.1 SuppressedError ( error, suppressed, message [ , options ] ), https://tc39.es/proposal-explicit-resource-management/#sec-suppressederror
ThrowCompletionOr<NonnullGCPtr<Object>> SuppressedErrorConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    auto error = vm.argument(0);
    auto suppressed = vm.argument(1);
    auto message = vm.argument(2);
    auto options = vm.argument(3);

    // 2. Let O be ? OrdinaryCreateFromConstructor(newTarget, "%SuppressedError.prototype%", « [[ErrorData]] »).
    auto suppressed_error = TRY(ordinary_create_from_constructor<SuppressedError>(vm, new_target, &Intrinsics::suppressed_error_prototype));

    // 3. If message is not undefined, then
    if (!message.is_undefined()) {
        // a. Let msg be ? ToString(message).
        auto msg = TRY(message.to_string(vm));

        // b. Perform CreateNonEnumerableDataPropertyOrThrow(O, "message", msg).
        suppressed_error->create_non_enumerable_data_property_or_throw(vm.names.message, PrimitiveString::create(vm, move(msg)));
    }

    // 4. Perform ? InstallErrorCause(O, options).
    TRY(suppressed_error->install_error_cause(options));

    // 5. Perform ! DefinePropertyOrThrow(O, "error", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: error }).
    MUST(suppressed_error->define_property_or_throw(vm.names.error, { .value = error, .writable = true, .enumerable = false, .configurable = true }));

    // 6. Perform ! DefinePropertyOrThrow(O, "suppressed", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: suppressed }).
    MUST(suppressed_error->define_property_or_throw(vm.names.suppressed, { .value = suppressed, .writable = true, .enumerable = false, .configurable = true }));

    // 7. Return O.
    return suppressed_error;
}

}
